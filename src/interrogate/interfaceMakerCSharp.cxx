/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file interfaceMakerCSharp.cxx
 * @date 2026-04-09
 */

#include "interfaceMakerCSharp.h"

#include "interrogate.h"
#include "interrogateBuilder.h"
#include "functionRemap.h"
#include "parameterRemap.h"
#include "parameterRemapToString.h"
#include "typeManager.h"

#include "interrogateDatabase.h"
#include "skipReport.h"
#include "interrogateElement.h"
#include "interrogateFunction.h"
#include "interrogateFunctionWrapper.h"
#include "interrogateManifest.h"
#include "interrogateMakeSeq.h"
#include "interrogateType.h"

#include "cppFunctionType.h"
#include "cppInstance.h"
#include "cppParameterList.h"
#include "cppStructType.h"
#include "filename.h"

#include <cctype>
#include <cstring>
#include <functional>
#include <fstream>
#include <set>
#include <sstream>

using std::ostream;
using std::string;

std::set<int> csharp_owned_type_indices;
std::map<int, std::string> csharp_type_module_map;
std::map<std::string, std::string> csharp_library_to_module;

// Cache for get_collection_canonical_library(): facade class name -> owner library.
std::map<std::string, std::string> csharp_collection_canonical_library;

// Remap for std::wstring parameters/returns in C# mode.
// Uses UTF-8 (char const *) at the C boundary instead of wchar_t const *,
// converting via TextEncoder::decode_text / encode_wtext so that
// StringMarshalling.Utf8 on the C# side round-trips correctly.
// The reason we can't reliably use wchar is because its size is different
// between Windows and Linux. Since the C# code is the same across
// platforms, we just use UTF-8 stored in char.
class ParameterRemapWStringCSharp : public ParameterRemap {
public:
  explicit ParameterRemapWStringCSharp(CPPType *orig_type) : ParameterRemap(orig_type) {
    static CPPType *const_char_star = nullptr;
    if (const_char_star == nullptr) {
      const_char_star = parser.parse_type("const char *");
    }
    _new_type = const_char_star;
  }

  void pass_parameter(std::ostream &out, const std::string &variable_name) override {
    out << "TextEncoder::decode_text(std::string(" << variable_name
        << "), TextEncoder::E_utf8)";
  }

  std::string prepare_return_expr(std::ostream &out, int indent_level,
                                  const std::string &expression) override {
    // thread_local so the c_str() pointer outlives the return but is
    // refreshed on every call; see ParameterRemapBasicStringToString.
    InterfaceMaker::indent(out, indent_level)
      << "thread_local std::string string_holder;\n";
    InterfaceMaker::indent(out, indent_level)
      << "string_holder = TextEncoder::encode_wtext("
      << expression << ", TextEncoder::E_utf8);\n";
    return "string_holder";
  }

  std::string get_return_expr(const std::string &) override {
    return "string_holder.c_str()";
  }

  AtomicToken get_new_atomic_token() override { return AT_string; }
};

// Factory called from interfaceMaker.cxx when build_csharp is set.
ParameterRemap *make_wstring_csharp_remap(CPPType *type) {
  return new ParameterRemapWStringCSharp(type);
}

// Remap for std::istream / std::ostream / std::iostream parameters in C# mode.
// The parameter crosses the C ABI as an opaque void* that points to a C++
// stream object created on the C# side (see Interrogate.StreamBridge).  The
// remap's job is to reinterpret_cast that void* back to the right stream
// pointer, dereferencing if the original declaration was a reference.
//
// new_type is void*; the parameter's database-recorded type is one of the
// AT_istream/AT_ostream/AT_iostream atomic tokens, letting the C# interface
// maker recognise the parameter as "this is a native-stream bridge" during
// pass 2 even after the .in database round-trip.
class ParameterRemapStreamCSharp : public ParameterRemap {
public:
  ParameterRemapStreamCSharp(CPPType *orig_type, AtomicToken token,
                             const char *cpp_stream_type)
      : ParameterRemap(orig_type), _token(token), _cpp_stream_type(cpp_stream_type) {
    static CPPType *void_ptr = nullptr;
    if (void_ptr == nullptr) {
      void_ptr = parser.parse_type("void *");
    }
    _new_type = void_ptr;

    // Remember whether the original parameter was passed by reference so that
    // pass_parameter() knows to dereference the void* back into a reference.
    _was_reference = TypeManager::is_reference(orig_type);
  }

  void pass_parameter(std::ostream &out, const std::string &variable_name) override {
    if (_was_reference) {
      out << "*reinterpret_cast<" << _cpp_stream_type << " *>("
          << variable_name << ")";
    } else {
      out << "reinterpret_cast<" << _cpp_stream_type << " *>("
          << variable_name << ")";
    }
  }

  // Cast the C++ stream expression (reference or pointer) to the void*
  // the C wrapper returns.  The C# side receives it as IntPtr — see
  // get_csharp_type — since we don't yet wrap returned streams back
  // into System.IO.Stream.
  std::string get_return_expr(const std::string &expression) override {
    if (_was_reference) {
      return "reinterpret_cast<void *>(&(" + expression + "))";
    }
    return "reinterpret_cast<void *>(" + expression + ")";
  }

  AtomicToken get_new_atomic_token() override { return _token; }

private:
  AtomicToken _token;
  const char *_cpp_stream_type;
  bool _was_reference;
};

// Factory called from interfaceMaker.cxx when build_csharp is set and the
// parameter's original C++ type is a pointer or reference to a std stream.
ParameterRemap *make_stream_csharp_remap(CPPType *type) {
  if (TypeManager::is_pointer_to_iostream(type)) {
    return new ParameterRemapStreamCSharp(type, AT_iostream, "std::iostream");
  }
  if (TypeManager::is_pointer_to_istream(type)) {
    return new ParameterRemapStreamCSharp(type, AT_istream, "std::istream");
  }
  if (TypeManager::is_pointer_to_ostream(type)) {
    return new ParameterRemapStreamCSharp(type, AT_ostream, "std::ostream");
  }
  return nullptr;
}

namespace {

string
make_csharp_identifier(const string &name) {
  if (name.substr(0, 8) == "operator") {
    string op = name.substr(8);
    while (!op.empty() && op[0] == ' ') op = op.substr(1);
    static const struct { const char *sym; const char *csharp; } op_map[] = {
      {"==", "op_eq"}, {"!=", "op_ne"}, {"<", "op_lt"}, {">", "op_gt"},
      {"<=", "op_le"}, {">=", "op_ge"}, {"<=>", "op_spaceship"},
      {"+", "op_add"}, {"-", "op_sub"},
      {"*", "op_mul"}, {"/", "op_div"}, {"%", "op_mod"}, {"&", "op_and"},
      {"|", "op_or"}, {"^", "op_xor"}, {"~", "op_inv"}, {"!", "op_not"},
      {"<<", "op_lshift"}, {">>", "op_rshift"}, {"+=", "op_iadd"},
      {"-=", "op_isub"}, {"*=", "op_imul"}, {"/=", "op_idiv"},
      {"[]", "op_index"}, {"()", "op_call"}, {"=", "op_assign"},
      {"++", "op_inc"}, {"--", "op_dec"},
      {"typecast operator", "op_typecast"},
      {"<=>", "op_spaceship"}, {"<<=", "op_ilshift"}, {">>=", "op_irshift"},
      {"&=", "op_iand"}, {"|=", "op_ior"}, {"^=", "op_ixor"},
      {"&&", "op_land"}, {"||", "op_lor"},
      {"->", "op_arrow"}, {"->*", "op_arrow_star"}, {",", "op_comma"},
      {"new", "op_new"}, {"delete", "op_delete"},
      {"bool", "op_bool"}, {"int", "op_int"}, {"float", "op_float"},
      {"double", "op_double"}, {"long", "op_long"},
      {"unsigned int", "op_uint"}, {"char", "op_char"},
      {"void *", "op_void_ptr"},
    };
    for (size_t i = 0; i < sizeof(op_map) / sizeof(op_map[0]); ++i) {
      if (op == op_map[i].sym) return string(op_map[i].csharp);
    }
    string cleaned = InterrogateBuilder::clean_identifier(op);
    if (!cleaned.empty()) return "op_" + cleaned;
    unsigned hash = 0;
    for (char c : op) hash = hash * 31 + (unsigned char)c;
    char buf[16];
    snprintf(buf, sizeof(buf), "op_%08x", hash);
    return string(buf);
  }

  string ident = InterrogateBuilder::clean_identifier(name);
  if (ident.empty()) {
    ident = "Value";
  }

  if (!ident.empty() && std::isdigit((unsigned char)ident[0])) {
    ident = "_" + ident;
  }

  static const char *const keywords[] = {
    "abstract", "as", "base", "bool", "break", "byte", "case", "catch",
    "char", "checked", "class", "const", "continue", "decimal", "default",
    "delegate", "do", "double", "else", "enum", "event", "explicit",
    "extern", "false", "finally", "fixed", "float", "for", "foreach",
    "goto", "if", "implicit", "in", "int", "interface", "internal", "is",
    "lock", "long", "namespace", "new", "null", "object", "operator",
    "out", "override", "params", "private", "protected", "public", "readonly",
    "ref", "return", "sbyte", "sealed", "short", "sizeof", "stackalloc",
    "static", "string", "struct", "switch", "this", "throw", "true", "try",
    "typeof", "uint", "ulong", "unchecked", "unsafe", "ushort", "using",
    "virtual", "void", "volatile", "while"
  };

  for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
    if (ident == keywords[i]) {
      ident = "_" + ident;
      break;
    }
  }

  return ident;
}

// Converts a C++ enum-value name to one idiomatic PascalCase C# identifier.
//   LEFT_X / left_x -> LeftX     NONE / none -> None     X / x -> X
//   M_off -> MOff                CS_zup_right -> CsZupRight
string
make_csharp_enum_member(const string &name) {
  bool has_underscore = name.find('_') != string::npos;
  bool all_upper = true;
  bool all_lower = true;
  for (char c : name) {
    if (std::isalpha((unsigned char)c)) {
      if (std::islower((unsigned char)c)) all_upper = false;
      if (std::isupper((unsigned char)c)) all_lower = false;
    }
  }

  string result;
  if (has_underscore || all_upper || all_lower) {
    // snake_case / FLAT_CASE: PascalCase each underscore-delimited word.
    bool new_word = true;
    for (char c : name) {
      if (c == '_') {
        new_word = true;
        continue;
      }
      if (new_word) {
        result += (char)std::toupper((unsigned char)c);
        new_word = false;
      } else {
        result += (char)std::tolower((unsigned char)c);
      }
    }
  } else {
    // mixed-case, no separators: keep internal casing, upper the first letter.
    result = name;
    if (!result.empty()) {
      result[0] = (char)std::toupper((unsigned char)result[0]);
    }
  }

  if (result.empty()) {
    result = "Value";
  }
  if (std::isdigit((unsigned char)result[0])) {
    result = "_" + result;
  }

  return make_csharp_identifier(result);
}

string
quote_csharp_string(const string &value) {
  string result;
  result.reserve(value.size() + 2);

  for (string::const_iterator si = value.begin(); si != value.end(); ++si) {
    switch (*si) {
    case '\\':
      result += "\\\\";
      break;

    case '"':
      result += "\\\"";
      break;

    case '\n':
      result += "\\n";
      break;

    case '\r':
      result += "\\r";
      break;

    case '\t':
      result += "\\t";
      break;

    default:
      result += *si;
    }
  }

  return result;
}

string
prettify_namespace(const string &module_name) {
  if (module_name.empty()) {
    return "Interrogate";
  }

  string result;
  bool capitalize_next = true;
  for (size_t i = 0; i < module_name.size(); ++i) {
    char c = module_name[i];
    if (c == '.' || c == '_') {
      if (!result.empty() && result.back() != '.') {
        result += '.';
      }
      capitalize_next = true;
    } else if (capitalize_next) {
      result += (char)std::toupper((unsigned char)c);
      capitalize_next = false;
    } else {
      // Capitalize letters that follow digits (e.g. "3d" -> "3D")
      if (std::isalpha((unsigned char)c) && !result.empty() &&
          std::isdigit((unsigned char)result.back())) {
        result += (char)std::toupper((unsigned char)c);
      } else {
        result += c;
      }
    }
  }

  return result;
}

// Name of the C# class holding a module's free functions.  Prettified
// namespace with dots removed: "panda3d.core" -> "Panda3DCoreGlobals".
string
get_globals_class_name(const string &module_name) {
  string pretty = prettify_namespace(module_name);
  string result;
  result.reserve(pretty.size() + 7);
  for (char c : pretty) {
    if (c != '.') {
      result += c;
    }
  }
  result += "Globals";
  return result;
}

string
to_pascal_case(const string &snake_name) {
  string result;
  bool capitalize_next = true;
  for (char c : snake_name) {
    if (c == '_') {
      capitalize_next = true;
    } else if (capitalize_next) {
      result += (char)std::toupper((unsigned char)c);
      capitalize_next = false;
    } else {
      result += c;
    }
  }
  return result;
}

string
trim_whitespace(const string &text) {
  size_t begin = 0;
  while (begin < text.size() && std::isspace((unsigned char)text[begin])) {
    ++begin;
  }

  size_t end = text.size();
  while (end > begin && std::isspace((unsigned char)text[end - 1])) {
    --end;
  }

  return text.substr(begin, end - begin);
}

string
escape_xml_doc_text(const string &text) {
  string result;
  result.reserve(text.size());

  for (char c : text) {
    switch (c) {
    case '&':
      result += "&amp;";
      break;

    case '<':
      result += "&lt;";
      break;

    case '>':
      result += "&gt;";
      break;

    default:
      result += c;
    }
  }

  return result;
}

string
normalize_xml_doc_line(const string &text) {
  string line = trim_whitespace(text);

  if (line == "*/") {
    return string();
  }

  if (line.compare(0, 3, "///") == 0) {
    line = line.substr(3);
  } else if (line.compare(0, 3, "/**") == 0) {
    line = line.substr(3);
  } else if (line.compare(0, 2, "/*") == 0) {
    line = line.substr(2);
  } else if (line.compare(0, 2, "//") == 0) {
    line = line.substr(2);
  } else if (!line.empty() && line[0] == '*') {
    line = line.substr(1);
  }

  line = trim_whitespace(line);

  if (line.size() >= 2 && line.compare(line.size() - 2, 2, "*/") == 0) {
    line = trim_whitespace(line.substr(0, line.size() - 2));
  }

  if (line == "/") {
    return string();
  }

  return line;
}

void
emit_xml_doc_comment(ostream &out, const string &comment, int indent_level) {
  std::istringstream stream(comment);
  std::vector<string> lines;
  string line;
  while (std::getline(stream, line)) {
    lines.push_back(escape_xml_doc_text(normalize_xml_doc_line(line)));
  }

  while (!lines.empty() && lines.front().empty()) {
    lines.erase(lines.begin());
  }
  while (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }
  if (lines.empty()) {
    lines.push_back(string());
  }

  InterfaceMaker::indent(out, indent_level) << "/// <summary>\n";
  for (const string &doc_line : lines) {
    InterfaceMaker::indent(out, indent_level) << "///";
    if (!doc_line.empty()) {
      out << ' ' << doc_line;
    }
    out << "\n";
  }
  InterfaceMaker::indent(out, indent_level) << "/// </summary>\n";
}

string
get_csharp_parameter_name(FunctionRemap *remap, size_t index) {
  if (remap != nullptr && index < remap->_parameters.size() &&
      !remap->_parameters[index]._name.empty()) {
    return make_csharp_identifier(remap->_parameters[index]._name);
  }

  return make_csharp_identifier(remap->get_parameter_name((int)index));
}

void
collect_peer_namespaces(const string &dir, std::set<string> &namespaces) {
  Filename csharp_dir(dir);
  vector_string files;
  csharp_dir.scan_directory(files);
  for (const string &f : files) {
    if (f.size() > 18 && f.substr(0, 14) == "NativeMethods_" &&
        f.substr(f.size() - 3) == ".cs") {
      Filename path(dir + "/" + f);
      path.set_text();
      std::ifstream in;
      if (path.open_read(in)) {
        string line;
        while (std::getline(in, line)) {
          if (line.compare(0, 10, "namespace ") == 0) {
            string ns = line.substr(10);
            size_t brace = ns.find('{');
            if (brace != string::npos) {
              ns = ns.substr(0, brace);
            }
            while (!ns.empty() && (ns.back() == ' ' || ns.back() == '{')) {
              ns.pop_back();
            }
            if (!ns.empty()) {
              namespaces.insert(ns);
            }
            break;
          }
        }
      }
    }
  }
}

void
emit_peer_namespace_usings(ostream &out, const string &cs_namespace, const string &dir) {
  std::set<string> peers;
  collect_peer_namespaces(dir, peers);

  // Also add namespaces from the --module-map (csharp_library_to_module),
  // which provides a complete set of known modules.
  for (const auto &entry : csharp_library_to_module) {
    string ns = prettify_namespace(entry.second);
    if (!ns.empty()) {
      peers.insert(ns);
    }
  }

  for (const string &ns : peers) {
    if (ns != cs_namespace) {
      out << "using " << ns << ";\n";
    }
  }
}

static bool
find_file_in_tree(const Filename &dir, const string &basename,
                  Filename &result, std::set<string> &visited) {
  Filename search_dir(dir);
  search_dir.make_absolute();
  search_dir.standardize();

  string dir_key = search_dir.to_os_generic();
  if (!visited.insert(dir_key).second || !search_dir.is_directory()) {
    return false;
  }

  vector_string contents;
  if (!search_dir.scan_directory(contents)) {
    return false;
  }

  for (const string &entry : contents) {
    if (entry == basename) {
      result = Filename(search_dir, entry);
      return true;
    }
  }

  for (const string &entry : contents) {
    if (entry == "." || entry == "..") continue;
    Filename child(search_dir, entry);
    if (child.is_directory() && find_file_in_tree(child, basename, result, visited)) {
      return true;
    }
  }
  return false;
}

Filename
find_database_file(const string &basename) {
  // Check the directory of the first loaded .in file
  Filename local(output_data_filename.get_dirname(), basename);
  if (local.exists()) {
    return local;
  }

  // Search configured search directories recursively
  for (const Filename &dir : database_search_dirs) {
    Filename result;
    std::set<string> visited;
    if (find_file_in_tree(dir, basename, result, visited)) {
      return result;
    }
  }

  return Filename();
}

Filename
make_output_filename(const string &dir, const string &basename) {
  Filename fn(dir + "/" + basename);
  fn.set_text();
  return fn;
}

bool
open_output_file(const string &dir, const string &basename, std::ofstream &out) {
  Filename fn = make_output_filename(dir, basename);
  if (!fn.open_write(out)) {
    nout << "Failed to open " << fn << " for writing.\n";
    return false;
  }

  out << "#nullable enable\n\n";

  return true;
}

CPPType *
unwrap_csharp_visible_type(CPPType *type) {
  if (type == nullptr) {
    return nullptr;
  }

  type = TypeManager::resolve_type(type);
  type = TypeManager::unwrap_const(type);
  type = TypeManager::unwrap_reference(type);
  type = TypeManager::resolve_type(type);

  if (TypeManager::is_pointer(type)) {
    type = TypeManager::unwrap_pointer(type);
    type = TypeManager::resolve_type(type);
    type = TypeManager::unwrap_const(type);
    type = TypeManager::resolve_type(type);
  }

  return type;
}

const InterrogateType *
find_interrogate_type(const InterfaceMaker::Objects &objects, CPPType *type) {
  CPPType *visible_type = unwrap_csharp_visible_type(type);
  if (visible_type == nullptr) {
    return nullptr;
  }

  string visible_name = visible_type->get_fully_scoped_name();

  InterfaceMaker::Objects::const_iterator oi;
  for (oi = objects.begin(); oi != objects.end(); ++oi) {
    InterfaceMaker::Object *object = (*oi).second;
    if (object == nullptr || object->_itype._cpptype == nullptr) {
      continue;
    }

    CPPType *candidate = TypeManager::resolve_type(object->_itype._cpptype);
    if (candidate == visible_type) {
      return &object->_itype;
    }

    if (candidate != nullptr &&
        candidate->get_fully_scoped_name() == visible_name) {
      return &object->_itype;
    }
  }

  return nullptr;
}

bool
is_wrapped_object_type(const InterfaceMaker::Objects &objects, CPPType *type) {
  const InterrogateType *itype = find_interrogate_type(objects, type);
  return itype != nullptr && (itype->is_class() || itype->is_struct());
}

bool
is_enum_type(const InterfaceMaker::Objects &objects, CPPType *type) {
  const InterrogateType *itype = find_interrogate_type(objects, type);
  return itype != nullptr && itype->is_enum();
}

FunctionRemap *
first_legal_remap(InterfaceMaker::Function *func) {
  if (func == nullptr) {
    return nullptr;
  }

  InterfaceMaker::Function::Remaps::const_iterator ri;
  for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
    FunctionRemap *remap = (*ri);
    if (
        (remap->_flags & FunctionRemap::F_explicit_self) == 0) {
      return remap;
    }
  }

  return nullptr;
}

string
build_signature_key(const string &name, const std::vector<string> &param_types,
                    bool is_static) {
  std::ostringstream strm;
  strm << (is_static ? 'S' : 'I') << ':' << name << '(';
  for (size_t i = 0; i < param_types.size(); ++i) {
    if (i != 0) {
      strm << ',';
    }
    strm << param_types[i];
  }
  strm << ')';
  return strm.str();
}

struct CSharpOperatorParam {
  string type;
  string name;
  bool nullable_reference;
  bool native_object;
};

struct CSharpOperatorCandidate {
  string method_name;
  string symbol;
  string return_type;
  std::vector<CSharpOperatorParam> parameters;
};

string
get_csharp_operator_symbol(const string &method_name) {
  static const struct { const char *method; const char *symbol; } op_map[] = {
    {"op_eq", "=="}, {"op_ne", "!="},
    {"op_lt", "<"}, {"op_gt", ">"}, {"op_le", "<="}, {"op_ge", ">="},
    {"op_add", "+"}, {"op_sub", "-"}, {"op_mul", "*"}, {"op_div", "/"},
    {"op_mod", "%"}, {"op_and", "&"}, {"op_or", "|"}, {"op_xor", "^"},
    {"op_inv", "~"}, {"op_not", "!"},
    {"op_lshift", "<<"}, {"op_rshift", ">>"},
    {"op_inc", "++"}, {"op_dec", "--"},
  };

  for (size_t i = 0; i < sizeof(op_map) / sizeof(op_map[0]); ++i) {
    if (method_name == op_map[i].method) {
      return op_map[i].symbol;
    }
  }
  return string();
}

bool
is_csharp_operator_value_type_name(const string &type_name) {
  static const char *const value_types[] = {
    "bool", "byte", "sbyte", "short", "ushort", "int", "uint",
    "long", "ulong", "float", "double", "IntPtr"
  };
  for (size_t i = 0; i < sizeof(value_types) / sizeof(value_types[0]); ++i) {
    if (type_name == value_types[i]) {
      return true;
    }
  }
  if (type_name == "string" || type_name == "string?") {
    return false;
  }

  // Generated native-object parameters are surfaced as interfaces (IFoo or
  // global::Namespace.IFoo).  Everything else that reaches this path is treated
  // as a value-like type for nullable-annotation purposes, which covers enums.
  size_t last_dot = type_name.rfind('.');
  size_t first = (last_dot == string::npos) ? 0 : last_dot + 1;
  return !(first < type_name.size() && type_name[first] == 'I');
}

string
make_nullable_operator_type(const CSharpOperatorParam &param) {
  if (!param.nullable_reference || param.type.empty() || param.type[param.type.size() - 1] == '?') {
    return param.type;
  }
  return param.type + "?";
}

string
strip_nullable_operator_type(const string &type_name) {
  if (!type_name.empty() && type_name[type_name.size() - 1] == '?') {
    return type_name.substr(0, type_name.size() - 1);
  }
  return type_name;
}

string
operator_operand_key(const CSharpOperatorCandidate &candidate) {
  std::ostringstream strm;
  strm << '(';
  for (size_t i = 0; i < candidate.parameters.size(); ++i) {
    if (i != 0) {
      strm << ',';
    }
    strm << candidate.parameters[i].type;
  }
  strm << ')';
  return strm.str();
}

string
operator_signature_key(const string &symbol, const CSharpOperatorCandidate &candidate) {
  return symbol + operator_operand_key(candidate);
}

bool
is_unary_csharp_operator(const string &symbol) {
  return symbol == "+" || symbol == "-" || symbol == "!" || symbol == "~" ||
         symbol == "++" || symbol == "--";
}

bool
is_binary_csharp_operator(const string &symbol) {
  return symbol == "+" || symbol == "-" || symbol == "*" || symbol == "/" ||
         symbol == "%" || symbol == "&" || symbol == "|" || symbol == "^" ||
         symbol == "<<" || symbol == ">>";
}

bool
is_equality_csharp_operator(const string &symbol) {
  return symbol == "==" || symbol == "!=";
}

bool
is_ordered_csharp_operator(const string &symbol) {
  return symbol == "<" || symbol == ">" || symbol == "<=" || symbol == ">=";
}

string
opposite_ordered_operator(const string &symbol) {
  if (symbol == "<") return ">";
  if (symbol == ">") return "<";
  if (symbol == "<=") return ">=";
  if (symbol == ">=") return "<=";
  return string();
}

TypeIndex get_type_index_for_interrogate_type(const InterrogateType &itype);

string
get_local_csharp_type_name(const InterrogateType &itype) {
  if (itype.has_name()) {
    return make_csharp_identifier(itype.get_name());
  }
  if (itype.has_scoped_name()) {
    return make_csharp_identifier(InterrogateBuilder::descope(itype.get_scoped_name()));
  }
  if (itype._cpptype != nullptr) {
    return make_csharp_identifier(itype._cpptype->get_local_name(&parser));
  }

  return string();
}

string
get_raw_type_name(const InterrogateType &itype) {
  if (itype.has_true_name()) {
    return itype.get_true_name();
  }
  if (itype.has_scoped_name()) {
    return itype.get_scoped_name();
  }
  if (itype.has_name()) {
    return itype.get_name();
  }
  if (itype._cpptype != nullptr) {
    return itype._cpptype->get_local_name(&parser);
  }

  return string();
}

bool
is_template_instantiation_name(const string &name) {
  string::size_type open = name.find('<');
  return open != string::npos && name.find('>', open + 1) != string::npos;
}

string
get_direct_typedef_csharp_type_name(const InterrogateType &itype) {
  if (!(itype.is_class() || itype.is_struct()) ||
      !is_template_instantiation_name(get_raw_type_name(itype))) {
    return string();
  }

  TypeIndex target_index = get_type_index_for_interrogate_type(itype);
  if (target_index == 0) {
    return string();
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  string best_name;
  int best_score = -1;

  int n = idb->get_num_all_types();
  for (int t = 0; t < n; ++t) {
    TypeIndex alias_index = idb->get_all_type(t);
    if (alias_index == 0 || alias_index == target_index) {
      continue;
    }

    const InterrogateType &alias = idb->get_type(alias_index);
    if (!alias.is_typedef() || alias.get_wrapped_type() != target_index) {
      continue;
    }
    if (!alias.is_global() && !alias.is_nested()) {
      continue;
    }

    string alias_name = get_local_csharp_type_name(alias);
    if (alias_name.empty() || alias_name.find('<') != string::npos) {
      continue;
    }

    int score = 100;
    if (alias_name.find("Native") == string::npos) {
      score += 10;
    }
    if (alias_name.find('_') == string::npos) {
      score += 2;
    }
    if (alias.has_scoped_name() || alias.has_name()) {
      score += 1;
    }

    if (score > best_score ||
        (score == best_score &&
         (best_name.empty() || alias_name.size() < best_name.size() ||
          (alias_name.size() == best_name.size() && alias_name < best_name)))) {
      best_name = alias_name;
      best_score = score;
    }
  }

  return best_name;
}

string
get_csharp_type_name(const InterrogateType &itype) {
  // For types nested inside another class, prefix the C# name with the outer
  // class name + underscore.
  if (itype.get_outer_class() != 0) {
    InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
    const InterrogateType &outer = idb->get_type(itype.get_outer_class());
    string outer_name;
    if (outer.has_name()) {
      outer_name = make_csharp_identifier(outer.get_name());
    } else if (outer.has_scoped_name()) {
      outer_name = make_csharp_identifier(InterrogateBuilder::descope(outer.get_scoped_name()));
    }
    string simple;
    if (itype.has_name()) {
      simple = make_csharp_identifier(itype.get_name());
    } else if (itype.has_scoped_name()) {
      simple = make_csharp_identifier(InterrogateBuilder::descope(itype.get_scoped_name()));
    }
    if (!outer_name.empty() && !simple.empty()) {
      return outer_name + "_" + simple;
    }
  }

  string typedef_name = get_direct_typedef_csharp_type_name(itype);
  if (!typedef_name.empty()) {
    return typedef_name;
  }

  string local_name = get_local_csharp_type_name(itype);
  if (!local_name.empty()) {
    return local_name;
  }

  return "UnnamedType";
}

enum CollectionFacadeKind {
  CF_none,
  CF_readonly_array,
  CF_mutable_array
};

// Template heads whose instantiations (and anything reaching them via
// typedef / inheritance / DF_pointer_to edges) are treated as
// collection facades.  std::vector is the only entry: panda3d's
// pvector, ReferenceCountedVector, etc. all inherit from it;
// PointerToArray<T> reaches it via the DF_pointer_to edge on its
// PointerToBase<ReferenceCountedVector<T>> base.
static const char *const kCollectionTemplates[] = {
  "std::vector",
};

// Template heads that are smart-pointer holders, not facades.  A type
// whose own true_name matches one of these is never itself emitted as
// IList<T>, though its DF_pointer_to edge is still followed when it
// appears as a base of another class (so PointerToArray<T> etc. can
// still be detected).
static const char *const kPointerHolderTemplates[] = {
  "PointerToBase",
  "PointerTo",
  "ConstPointerTo",
};

// Extracts the first template argument from a true_name like
// "std::vector< int >" or "std::vector< std::string, allocator<...> >",
// tracking angle-bracket depth so nested templates don't split early.
// Returns whitespace-stripped; empty if true_name isn't a template.
static string extract_first_template_argument(const string &true_name) {
  string::size_type open = true_name.find('<');
  if (open == string::npos) {
    return string();
  }
  int depth = 1;
  string::size_type i = open + 1;
  string::size_type arg_end = string::npos;
  for (; i < true_name.size(); ++i) {
    char c = true_name[i];
    if (c == '<') {
      ++depth;
    } else if (c == '>') {
      --depth;
      if (depth == 0) {
        arg_end = i;
        break;
      }
    } else if (c == ',' && depth == 1) {
      arg_end = i;
      break;
    }
  }
  if (arg_end == string::npos) {
    return string();
  }
  string arg = true_name.substr(open + 1, arg_end - (open + 1));
  while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t')) arg.erase(arg.begin());
  while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\t')) arg.pop_back();
  return arg;
}

// Returns true if true_name's template head (bit before '<') matches
// one of `templates`, either fully ("std::vector") or as the simple
// name after the last "::" ("PointerTo" matches "NS::PointerTo<T>").
// Mirrors the simple-name check in TypeManager::is_pointer_to_base.
static bool matches_collection_template(const string &true_name,
                                        const char *const *templates,
                                        size_t templates_count) {
  string::size_type lt = true_name.find('<');
  if (lt == string::npos) {
    return false;
  }
  string head = true_name.substr(0, lt);
  // Simple name: whatever comes after the last "::".
  string::size_type colon = head.rfind("::");
  string simple = (colon == string::npos) ? head : head.substr(colon + 2);

  for (size_t i = 0; i < templates_count; ++i) {
    const char *tmpl = templates[i];
    if (head == tmpl || simple == tmpl) {
      return true;
    }
  }
  return false;
}

static TypeIndex unwrap_type_aliases(TypeIndex type_index);

// Walks itype's typedef chain (via _wrapped_type) and then its
// derivations (real bases + DF_pointer_to), looking for an instantiation
// of a kCollectionTemplates entry.  On a hit, writes the element-type
// name to element_true_name_out and returns CF_mutable_array; the
// caller refines mutable-vs-readonly via collection_facade_is_readonly.
// Uses only serialized fields so pass 1 and pass 2 agree.
// crossed_pointer_to_out reports whether the winning path traversed a
// DF_pointer_to edge.  That is the difference between "this type IS-A vector"
// (typedefs and plain public inheritance — std::vector's members come with it,
// whether or not the db recorded any) and "this type merely HOLDS a vector"
// (a smart-pointer holder, which only behaves like one if it forwards the
// methods itself).  The two need different trust rules; see
// detect_collection_facade_kind and collection_facade_is_readonly.
static CollectionFacadeKind walk_for_vector_base(
    const InterrogateType &itype,
    string &element_true_name_out,
    bool &crossed_pointer_to_out) {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  constexpr size_t N = sizeof(kCollectionTemplates) / sizeof(kCollectionTemplates[0]);

  // Peel typedef / pointer / const wrapping to reach the underlying type
  // (vector_int, vector_int *, const std::vector<int> &, etc.).
  const InterrogateType *cur = &itype;
  int guard = 32;
  while (guard-- > 0) {
    if (matches_collection_template(cur->_true_name,
                                    kCollectionTemplates, N)) {
      element_true_name_out = extract_first_template_argument(cur->_true_name);
      if (element_true_name_out.empty()) {
        return CF_none;
      }
      crossed_pointer_to_out = false;
      return CF_mutable_array;
    }
    if ((cur->is_typedef() || cur->is_wrapped() || cur->is_pointer()) &&
        cur->_wrapped_type != 0) {
      TypeIndex inner = unwrap_type_aliases(cur->_wrapped_type);
      if (inner == 0) break;
      cur = &idb->get_type(inner);
    } else {
      break;
    }
  }

  // Walk derivations — real bases and DF_pointer_to edges alike — so
  // smart-pointer wrappers transit to their pointee.  E.g.
  // PointerToArray<T> → PointerToBase<ReferenceCountedVector<T>> →
  // (DF_pointer_to) → ReferenceCountedVector<T> → pvector<T> →
  // std::vector<T>, all from the DB.
  for (int i = 0; i < cur->number_of_derivations(); ++i) {
    TypeIndex base_index = cur->get_derivation(i);
    if (base_index == 0) continue;
    const InterrogateType &base = idb->get_type(base_index);

    string element;
    bool crossed = false;
    CollectionFacadeKind kind = walk_for_vector_base(base, element, crossed);
    if (kind != CF_none) {
      element_true_name_out = element;
      crossed_pointer_to_out = crossed || cur->derivation_is_pointer_to(i);
      return kind;
    }
  }
  return CF_none;
}

// Does itype have a method (own or inherited) with this name?  Used to
// weed out abstract intermediates like PointerToArrayBase<T> that reach
// std::vector but don't themselves expose size() / operator[].  Walks
// only real C++ inheritance: methods reachable only via p()->foo() on a
// DF_pointer_to edge aren't callable on the holder.
static bool type_declares_method(const InterrogateType &itype, const string &name) {
  // Non-recursive: only the type's own methods, not inherited ones.
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  for (int i = 0; i < itype.number_of_methods(); ++i) {
    FunctionIndex fi = itype.get_method(i);
    if (fi == 0) continue;
    const InterrogateFunction &func = idb->get_function(fi);
    if (func.has_name() && func.get_name() == name) {
      return true;
    }
  }
  return false;
}

static bool type_has_method(const InterrogateType &itype, const string &name) {
  if (type_declares_method(itype, name)) {
    return true;
  }
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  for (int i = 0; i < itype.number_of_derivations(); ++i) {
    if (itype.derivation_is_pointer_to(i)) continue;
    TypeIndex base_index = itype.get_derivation(i);
    if (base_index == 0) continue;
    if (type_has_method(idb->get_type(base_index), name)) {
      return true;
    }
  }
  return false;
}

// True iff itype (or a real-inheritance ancestor) has a
// DF_pointer_to_const edge, meaning pass 1 saw a p() returning
// `const T *`.  Positive answer is authoritative (readonly); negative
// isn't, since p() may have been unpublished.
static bool has_pointer_to_const_edge(const InterrogateType &itype) {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  for (int i = 0; i < itype.number_of_derivations(); ++i) {
    if (itype.derivation_is_pointer_to_const(i)) {
      return true;
    }
  }
  for (int i = 0; i < itype.number_of_derivations(); ++i) {
    if (itype.derivation_is_pointer_to(i)) continue;
    TypeIndex base_index = itype.get_derivation(i);
    if (base_index == 0) continue;
    if (has_pointer_to_const_edge(idb->get_type(base_index))) {
      return true;
    }
  }
  return false;
}

// Classifies a collection facade as readonly vs mutable.
//
// Primary: DF_pointer_to_const, set in pass 1 when p() returned
// `const T *`.  Authoritative when p() was visible (PUBLISHED or
// -promiscuous).
//
// Fallback: infer from the method set — a readonly wrapper won't
// expose push_back / set_element.  Only trusted for a type that merely *holds*
// a vector (reached across a DF_pointer_to edge), which has to forward the
// methods to behave like one.  A type that IS-A vector inherits them, and its
// db record may list none at all — an external std::vector typedef and
// panda3d's empty-bodied pvector<T> both do — so the absence of push_back
// there says nothing, and trusting it would falsely mark them read-only.
static bool collection_facade_is_readonly(const InterrogateType &itype,
                                          bool is_a_vector) {
  if (has_pointer_to_const_edge(itype)) {
    return true;
  }
  if (is_a_vector) {
    return false;
  }
  return !type_has_method(itype, "push_back") &&
         !type_has_method(itype, "set_element");
}

bool should_skip_csharp_type(const InterrogateType &itype);

// Is this type const-qualified?  The native helper writer decides this from the
// C++ type name having a trailing "const" (it has the CPPType; pass 2 does not),
// and the two must agree: if it says const it emits no push_back/resize helpers,
// so anything here that calls the type mutable would generate a NativeList<T>
// bound to entry points that were never written.  Mirror its test exactly.
static bool is_const_qualified_type(const InterrogateType &itype) {
  // Look through pointer / reference layers to the pointee: a facade object is
  // usually reached as `T const *` (that is what `const vector_uchar &` becomes
  // once the reference is remapped to a pointer).  The pointer itself is not
  // const -- the thing it points at is -- so testing only the outer type finds
  // nothing, while the native writer, which names the pointee, sees the const
  // and drops the mutating helpers.  Walk the raw _wrapped_type chain, NOT
  // unwrap_type_aliases: that strips const, which is the very thing we're after.
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateType *cur = &itype;

  for (int guard = 0; guard < 32; ++guard) {
    if (cur->is_const()) {
      return true;
    }
    const string &name = cur->_true_name;
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, "const") == 0) {
      return true;
    }
    if ((cur->is_pointer() || cur->is_wrapped()) && cur->_wrapped_type != 0) {
      cur = &idb->get_type(cur->_wrapped_type);
    } else {
      break;
    }
  }
  return false;
}

// A PointerTo<T> / ConstPointerTo<T> element is a refcounted *handle*, not a value:
// AsyncFuture::Futures is pvector<PT(AsyncFuture)>.  Returns T's C++ name, or "".
//
// It matters because the synthesized element accessor heap-copies a value element
// (`new e_cpp_type((*self)[i])`), and doing that to a smart pointer allocates a
// PointerTo object -- which is not what the managed side unwraps.  A handle has to
// cross as the pointer it already is.
static string collection_element_pointee(const string &element_cpp_name) {
  string::size_type lt = element_cpp_name.find('<');
  if (lt == string::npos) {
    return string();
  }
  string head = element_cpp_name.substr(0, lt);
  while (!head.empty() && head.back() == ' ') {
    head.pop_back();
  }
  string::size_type colon = head.rfind("::");
  string simple = (colon == string::npos) ? head : head.substr(colon + 2);
  if (simple != "PointerTo" && simple != "ConstPointerTo") {
    return string();
  }
  return extract_first_template_argument(element_cpp_name);
}

// Can C# name this element type at all?
//
// get_collection_element_type_from_cpp_name falls back to mangling the C++ name
// when it recognises nothing, so an element the generator never emits still
// yields a plausible-looking identifier — InputDevice::ButtonState becomes
// `InputDevice_ButtonState`, a class that is written nowhere.  The facade then
// references a type that does not exist and the bindings do not compile.  A
// container we cannot express is better left unexposed (and the skip report now
// says so) than turned into a class that breaks the build.
static bool collection_element_is_expressible(const string &element_cpp_name) {
  string clean = element_cpp_name;
  while (!clean.empty() && (clean.front() == ' ' || clean.front() == '\t')) {
    clean.erase(clean.begin());
  }
  while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\t')) {
    clean.pop_back();
  }
  if (clean.empty()) {
    return false;
  }

  // A PT(T) element is expressible exactly when T is.
  string pointee = collection_element_pointee(clean);
  if (!pointee.empty()) {
    return collection_element_is_expressible(pointee);
  }

  // Must mirror get_collection_element_type_from_cpp_name's primitive table.
  static const char *const primitives[] = {
    "unsigned char", "signed char", "char",
    "unsigned short", "unsigned short int", "short", "short int",
    "unsigned int", "unsigned", "int", "long", "long int",
    "unsigned long", "unsigned long int",
    "long long", "long long int", "unsigned long long", "unsigned long long int",
    "float", "double", "bool",
    "std::string", "string", "std::wstring", "wstring",
  };
  for (const char *prim : primitives) {
    if (clean == prim) {
      return true;
    }
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  TypeIndex ti = idb->lookup_type_by_true_name(clean);
  if (ti == 0) {
    ti = idb->lookup_type_by_scoped_name(clean);
  }
  if (ti == 0) {
    ti = idb->lookup_type_by_name(clean);
  }
  if (ti == 0) {
    return false;
  }
  ti = unwrap_type_aliases(ti);
  if (ti == 0) {
    return false;
  }
  const InterrogateType &etype = idb->get_type(ti);
  if (!should_skip_csharp_type(etype)) {
    return true;
  }

  // A cross-module element type is only a foreign stub in pass 1 (its publishing
  // module's .in merges in pass 2), so should_skip_csharp_type() rejects it and
  // the Collection_* native helpers are never emitted -- yet pass 2 still emits
  // the facade.  In pass 1, accept a named non-std:: class/struct/enum stub so
  // the helpers are emitted; pass 2 keeps the strict test.
  if (!csharp_database_only_pass &&
      (etype.is_class() || etype.is_struct() || etype.is_enum())) {
    string scoped = etype.has_scoped_name() ? etype.get_scoped_name() : string();
    if (scoped.compare(0, 5, "std::") != 0) {
      return true;
    }
  }
  return false;
}

static CollectionFacadeKind detect_collection_facade_kind(
    const InterrogateType &itype,
    string &element_true_name_out) {
  constexpr size_t NP = sizeof(kPointerHolderTemplates) / sizeof(kPointerHolderTemplates[0]);

  // Reject the smart-pointer holders themselves — their DF_pointer_to
  // edge can reach std::vector, but only subclasses that forward vector
  // methods (PointerToArray<T>, FancyArray<T>) should surface as IList<T>.
  if (matches_collection_template(itype._true_name,
                                  kPointerHolderTemplates, NP)) {
    return CF_none;
  }


  bool crossed_pointer_to = false;
  CollectionFacadeKind kind =
    walk_for_vector_base(itype, element_true_name_out, crossed_pointer_to);
  if (kind == CF_none) {
    return CF_none;
  }

  if (!collection_element_is_expressible(element_true_name_out)) {
    return CF_none;
  }

  // Reaching std::vector through typedefs and plain public inheritance means
  // the type IS-A vector: size() / operator[] / push_back come with the base,
  // whether or not the database happens to record them.  Demanding a recorded
  // size() here is wrong, and it is what dropped every vector_uchar method --
  // panda3d gives pvector<T> an empty body under CPPPARSER ("simplified
  // definition to speed up Interrogate parsing"), so `pvector<T> : std::vector<T>`
  // carries no methods at all in the db.
  //
  // Crossing a DF_pointer_to edge is the case that needs the check: a
  // smart-pointer holder only behaves like a vector if it forwards the methods,
  // and abstract intermediates like PointerToArrayBase<T> reach std::vector
  // without exposing anything.
  const bool is_a_vector = !crossed_pointer_to;

  if (!is_a_vector && !type_has_method(itype, "size")) {
    return CF_none;
  }

  // A const-qualified vector is read-only, and must be classified so here: the
  // native helper writer already refuses to emit the mutating helpers for it
  // (you cannot call push_back / resize on a const T).  Calling it mutable would
  // emit a NativeList<T> whose resize() entry point was never generated -- an
  // EntryPointNotFoundException on first use.
  if (is_const_qualified_type(itype) ||
      collection_facade_is_readonly(itype, is_a_vector)) {
    return CF_readonly_array;
  }

  return kind;
}

static CollectionFacadeKind get_collection_facade_kind(const InterrogateType &itype) {
  string unused;
  return detect_collection_facade_kind(itype, unused);
}

static bool is_collection_facade_type(const InterrogateType &itype) {
  return get_collection_facade_kind(itype) != CF_none;
}

static bool is_collection_type_index(TypeIndex type_index) {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  if (type_index == 0) return false;
  // detect_collection_facade_kind already peels pointer / reference / const
  // / typedef wrapping and walks the inheritance chain, so we don't need a
  // second pass here.
  return is_collection_facade_type(idb->get_type(type_index));
}

bool
has_pointer_facade_name(const InterrogateType &itype) {
  string simple_name = get_csharp_type_name(itype);
  return simple_name.compare(0, 15, "ConstPointerTo_") == 0 ||
         simple_name.compare(0, 10, "PointerTo_") == 0;
}

TypeIndex get_type_index_for_interrogate_type(const InterrogateType &itype);
TypeIndex get_type_index_for_cpp_type(CPPType *type, CPPScope *scope);
bool is_csharp_primitive_type(const string &type_name);
bool is_csharp_enum_type(TypeIndex type_index);
bool is_csharp_native_object_type(TypeIndex type_index, const string &type_name);

bool
is_wrapped_interrogate_type(TypeIndex type_index) {
  if (type_index == 0) {
    return false;
  }
  const InterrogateType &itype = InterrogateDatabase::get_ptr()->get_type(type_index);
  return itype.is_class() || itype.is_struct();
}

InterfaceMaker::Function *
find_method_on_type(InterfaceMakerCSharp *maker, const InterrogateType &itype,
                    const string &name) {
  for (int mi = 0; mi < itype.number_of_methods(); ++mi) {
    InterfaceMaker::Function *method = maker->record_function(itype, itype.get_method(mi));
    if (method != nullptr && method->_ifunc.has_name() && method->_ifunc.get_name() == name) {
      return method;
    }
  }
  return nullptr;
}

InterfaceMaker::Function *
find_method_in_database(InterfaceMakerCSharp *maker, const InterrogateType &itype,
                        const string &name) {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  TypeIndex type_index = get_type_index_for_interrogate_type(itype);
  if (type_index == 0) {
    return nullptr;
  }

  int num_functions = idb->get_num_all_functions();
  for (int i = 0; i < num_functions; ++i) {
    FunctionIndex func_index = idb->get_all_function(i);
    if (func_index == 0) {
      continue;
    }
    const InterrogateFunction &ifunc = idb->get_function(func_index);
    if (!ifunc.is_method() || ifunc.get_class() != type_index ||
        !ifunc.has_name() || ifunc.get_name() != name) {
      continue;
    }
    return maker->record_function(itype, func_index);
  }

  return nullptr;
}

InterfaceMaker::Function *
find_method_on_type_recursive(InterfaceMakerCSharp *maker, const InterrogateType &itype,
                              const string &name, std::set<TypeIndex> &visited) {
  TypeIndex current_index = get_type_index_for_interrogate_type(itype);
  if (current_index != 0 && !visited.insert(current_index).second) {
    return nullptr;
  }

  if (InterfaceMaker::Function *method = find_method_on_type(maker, itype, name)) {
    return method;
  }
  if (InterfaceMaker::Function *method = find_method_in_database(maker, itype, name)) {
    return method;
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  for (int di = 0; di < itype.number_of_derivations(); ++di) {
    TypeIndex base_index = itype.get_derivation(di);
    if (base_index == 0 || !is_wrapped_interrogate_type(base_index)) {
      continue;
    }
    const InterrogateType &base_type = idb->get_type(base_index);
    if (InterfaceMaker::Function *method = find_method_on_type_recursive(maker, base_type, name, visited)) {
      return method;
    }
  }

  return nullptr;
}

string
marshal_managed_argument(TypeIndex param_type_index, const string &param_type,
                         const string &param_name) {
  if (is_csharp_native_object_type(param_type_index, param_type) ||
      is_collection_type_index(param_type_index)) {
    if (!param_type.empty() && param_type[0] == 'I' && param_type.size() > 1 && isupper(param_type[1])) {
      // Strip nullable '?' suffix — 'as T?' is illegal in C#
      string as_type = param_type;
      if (!as_type.empty() && as_type.back() == '?') {
        as_type.pop_back();
      }
      return "(" + param_name + " as " + as_type + ")?.NativeHandle ?? IntPtr.Zero";
    } else {
      return "NativeObject.Unwrap(" + param_name + ")";
    }
  }
  if (!is_csharp_primitive_type(param_type) && is_csharp_enum_type(param_type_index)) {
    return "(int)" + param_name;
  }
  return param_name;
}

// If param_type_index refers to an atomic stream token, returns the
// corresponding AtomicToken.  Otherwise returns AT_not_atomic.
AtomicToken
csharp_stream_token_for_type(TypeIndex param_type_index) {
  if (param_type_index == 0) {
    return AT_not_atomic;
  }
  const InterrogateType &itype =
    InterrogateDatabase::get_ptr()->get_type(param_type_index);
  if (!itype.is_atomic()) {
    return AT_not_atomic;
  }
  AtomicToken tok = itype.get_atomic_token();
  if (tok == AT_istream || tok == AT_ostream || tok == AT_iostream) {
    return tok;
  }
  return AT_not_atomic;
}

// Name of the Interrogate.NativeStreamKind for a returned C++ stream.
static const char *csharp_native_stream_kind(AtomicToken token) {
  switch (token) {
  case AT_istream:  return "global::Interrogate.NativeStreamKind.Input";
  case AT_ostream:  return "global::Interrogate.NativeStreamKind.Output";
  case AT_iostream: return "global::Interrogate.NativeStreamKind.InputOutput";
  default:          return nullptr;
  }
}

// Name of the Interrogate.StreamBridge factory for a given direction.
const char *
csharp_stream_bridge_factory(AtomicToken tok) {
  switch (tok) {
  case AT_istream:  return "global::Interrogate.StreamBridge.ForInput";
  case AT_ostream:  return "global::Interrogate.StreamBridge.ForOutput";
  case AT_iostream: return "global::Interrogate.StreamBridge.ForInputOutput";
  default:          return nullptr;
  }
}

bool
has_param_value_helper_name(const InterrogateType &itype) {
  string simple_name = get_csharp_type_name(itype);
  return simple_name.compare(0, 11, "ParamValue_") == 0;
}

CPPType *
get_pointer_facade_pointee_cpp_type(const InterrogateType &itype) {
  if (!has_pointer_facade_name(itype)) {
    return nullptr;
  }

  if (itype._cpptype == nullptr) {
    return nullptr;
  }

  CPPType *resolved = TypeManager::resolve_type(itype._cpptype);
  CPPStructType *struct_type = resolved ? resolved->as_struct_type() : nullptr;
  if (struct_type == nullptr) {
    return nullptr;
  }

  CPPType *inner_ptr = TypeManager::get_pointer_type(struct_type);
  if (inner_ptr == nullptr) {
    return nullptr;
  }

  CPPType *inner = TypeManager::unwrap_pointer(inner_ptr);
  if (inner != nullptr) {
    inner = TypeManager::unwrap_const(inner);
  }
  return TypeManager::resolve_type(inner);
}

bool
is_empty_pointer_facade_type(const InterrogateType &itype) {
  return has_pointer_facade_name(itype) || has_param_value_helper_name(itype);
}

string
unwrap_pointer_facade_name(const string &name) {
  if (name.compare(0, 15, "ConstPointerTo_") == 0) {
    return name.substr(15);
  }
  if (name.compare(0, 10, "PointerTo_") == 0) {
    return name.substr(10);
  }
  return name;
}

string
get_pointer_facade_target_name(const InterrogateType &itype) {
  CPPType *inner = get_pointer_facade_pointee_cpp_type(itype);
  if (inner != nullptr) {
    return make_csharp_identifier(inner->get_local_name(&parser));
  }

  return unwrap_pointer_facade_name(get_csharp_type_name(itype));
}

bool
should_skip_csharp_type(const InterrogateType &itype) {
  if (is_empty_pointer_facade_type(itype)) {
    return true;
  }

  // A collection facade's C# form is synthesized from its element type
  // (NativeList<T> / NativeReadOnlyList<T>), so it needs no members of its own
  // to be expressible -- and must not be judged by the test below.  panda3d's
  // pvector<T> is deliberately an empty body under CPPPARSER, making it neither
  // global nor fully defined; skipping it here is what silently dropped every
  // vector_uchar and vector_string method (Datagram blobs, BAM encode/decode,
  // Multifile::read_subfile, vertex-buffer bytes, ...).
  if (is_collection_facade_type(itype)) {
    return false;
  }

  // Skip C++ internal types that are not exported.;
  if (!itype.is_global() && !itype.is_fully_defined()) {
    return true;
  }

  string simple_name = get_csharp_type_name(itype);
  if (simple_name == "basic_string_char") {
    return true;
  }
  if (itype.has_name() && itype.get_name() == "basic_string_char") {
    return true;
  }

  string scoped_name;
  if (itype.has_scoped_name()) {
    scoped_name = itype.get_scoped_name();
  } else if (itype._cpptype != nullptr) {
    CPPType *cpptype = TypeManager::resolve_type(itype._cpptype);
    if (cpptype != nullptr) {
      scoped_name = cpptype->get_fully_scoped_name();
    }
  }

  if (!scoped_name.empty() && scoped_name.compare(0, 5, "std::") == 0) {
    if (scoped_name.find("basic_string") != string::npos ||
        scoped_name.find("allocator") != string::npos ||
        scoped_name.find("char_traits") != string::npos ||
        scoped_name.find("initializer_list") != string::npos) {
      return true;
    }
  }

  return false;
}

bool
is_skipped_csharp_name(const string &name) {
  return name == "basic_string_char";
}

bool
is_csharp_type_legal(CPPType *in_ctype) {
  if (in_ctype == nullptr) {
    return false;
  }

  CPPType *type = TypeManager::resolve_type(in_ctype);
  if (TypeManager::is_rvalue_reference(type)) {
    return false;
  }

  // Stream types are allowed in both pointer and reference form; the C#
  // backend bridges them to System.IO.Stream via StreamBridge.  Recognise
  // these before the generic std::-prefix rejection below.
  if (TypeManager::is_pointer_to_istream(in_ctype) ||
      TypeManager::is_pointer_to_ostream(in_ctype) ||
      TypeManager::is_pointer_to_iostream(in_ctype)) {
    return true;
  }

  type = TypeManager::unwrap(type);

  if (TypeManager::is_void(type) ||
      TypeManager::is_basic_string_char(type) ||
      TypeManager::is_basic_string_wchar(type) ||
      TypeManager::is_simple(type) ||
      TypeManager::is_pointer_to_simple(type) ||
      TypeManager::is_bool(type) ||
      TypeManager::is_enum(type)) {
    return true;
  }

  string local_name = type->get_local_name(&parser);
  if (is_skipped_csharp_name(local_name)) {
    return false;
  }

  string scoped_name = type->get_fully_scoped_name();
  if (!scoped_name.empty() && scoped_name.compare(0, 5, "std::") == 0) {
    return false;
  }

  if (TypeManager::is_exported(type)) {
    return true;
  }

  if (TypeManager::is_pointer(type) || TypeManager::is_reference(type)) {
    CPPType *inner = TypeManager::unwrap(type);
    if (inner != nullptr) {
      string inner_name = inner->get_local_name(&parser);
      if (is_skipped_csharp_name(inner_name)) {
        return false;
      }
      string inner_scoped = inner->get_fully_scoped_name();
      if (!inner_scoped.empty() && inner_scoped.compare(0, 5, "std::") == 0) {
        return false;
      }
      if (TypeManager::is_exported(inner)) {
        return true;
      }
    }
  }

  return false;
}

bool
is_remap_legal_csharp(FunctionRemap *remap) {
  if (remap == nullptr || remap->_ForcedVoidReturn) {
    return false;
  }
  if (remap->_extension && !remap->_csharp_extension) {
    return false;
  }
  if ((remap->_flags & FunctionRemap::F_explicit_self) != 0) {
    return false;
  }

  if (!remap->_void_return) {
    if (!is_csharp_type_legal(remap->_return_type->get_orig_type()) ||
        !is_csharp_type_legal(remap->_return_type->get_new_type())) {
      return false;
    }
  }

  for (size_t i = 0; i < remap->_parameters.size(); ++i) {
    CPPType *orig_type = remap->_parameters[i]._remap->get_orig_type();
    CPPType *new_type = remap->_parameters[i]._remap->get_new_type();
    if (!is_csharp_type_legal(orig_type) || !is_csharp_type_legal(new_type)) {
      return false;
    }
  }

  return true;
}

bool
is_function_legal_csharp(InterfaceMaker::Function *func) {
  if (func == nullptr) {
    return false;
  }
  InterfaceMaker::Function::Remaps::const_iterator ri;
  for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
    if (is_remap_legal_csharp(*ri)) {
      return true;
    }
  }
  return false;
}

bool
is_abstract_type(const InterrogateType &itype) {
  if (itype.is_abstract()) {
    return true;
  }

  if (itype._cpptype == nullptr) {
    return false;
  }

  CPPType *cpptype = TypeManager::resolve_type(itype._cpptype);
  CPPStructType *struct_type = (cpptype != nullptr) ? cpptype->as_struct_type() : nullptr;
  return struct_type != nullptr && struct_type->is_abstract();
}

bool
is_csharp_primitive_type(const string &type_name) {
  static const char *const primitive_types[] = {
    "void", "bool", "byte", "sbyte", "short", "ushort", "int", "uint",
    "long", "ulong", "float", "double", "string", "IntPtr"
  };

  for (size_t i = 0; i < sizeof(primitive_types) / sizeof(primitive_types[0]); ++i) {
    if (type_name == primitive_types[i]) {
      return true;
    }
  }

  return false;
}

// Returns true for types that can be safely memcpy'd / span'd between
// managed and native memory (same binary layout, no marshaling needed).
bool
is_csharp_blittable_type(const string &type_name) {
  static const char *const blittable_types[] = {
    "byte", "sbyte", "short", "ushort", "int", "uint",
    "long", "ulong", "float", "double"
  };

  for (size_t i = 0; i < sizeof(blittable_types) / sizeof(blittable_types[0]); ++i) {
    if (type_name == blittable_types[i]) {
      return true;
    }
  }

  return false;
}


bool
is_csharp_native_object_type(const InterfaceMaker::Objects &objects, CPPType *type,
                             const string &type_name) {
  if (is_csharp_primitive_type(type_name)) {
    return false;
  }
  if (type != nullptr && is_enum_type(objects, type)) {
    return false;
  }
  if (type != nullptr) {
    CPPType *unwrapped = TypeManager::unwrap(TypeManager::resolve_type(type));
    if (TypeManager::is_enum(unwrapped)) {
      return false;
    }
  }
  return true;
}

string
get_native_ownership_name(FunctionRemap *remap, bool for_constructor) {
  if (remap != nullptr && remap->_manage_reference_count) {
    return "NativeOwnership.RefCounted";
  }
  if (for_constructor ||
      (remap != nullptr && remap->_return_value_needs_management)) {
    return "NativeOwnership.Owned";
  }
  return "NativeOwnership.Borrowed";
}

/**
 * Walks the InterrogateType inheritance chain to determine whether the given
 * type derives from ReferenceCount (or is ReferenceCount itself).
 */
static bool
is_type_refcounted(TypeIndex type_index,
                   std::set<TypeIndex> &visited) {
  if (type_index == 0) return false;
  if (!visited.insert(type_index).second) return false;  // cycle guard

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateType &itype = idb->get_type(type_index);

  const string &true_name = itype.get_true_name();
  if (true_name == "ReferenceCount" || true_name == "TypedReferenceCount") {
    return true;
  }

  int num_derivations = itype.number_of_derivations();
  for (int i = 0; i < num_derivations; ++i) {
    if (is_type_refcounted(itype.get_derivation(i), visited)) {
      return true;
    }
  }
  return false;
}

static bool
is_type_refcounted(TypeIndex type_index) {
  std::set<TypeIndex> visited;
  return is_type_refcounted(type_index, visited);
}

string
get_native_ownership_name(const InterrogateFunctionWrapper &wrapper,
                          bool for_constructor) {
  if (wrapper.manages_reference_count()) {
    return "NativeOwnership.RefCounted";
  }

  if (for_constructor || wrapper.caller_manages_return_value()) {
    return "NativeOwnership.Owned";
  }
  return "NativeOwnership.Borrowed";
}

const InterrogateFunctionWrapper *
get_wrapper_for_remap(FunctionRemap *remap) {
  if (remap == nullptr || remap->_wrapper_index == 0) {
    return nullptr;
  }

  return &InterrogateDatabase::get_ptr()->get_wrapper(remap->_wrapper_index);
}

bool
is_return_nullable(FunctionRemap *remap) {
  const InterrogateFunctionWrapper *wrapper = get_wrapper_for_remap(remap);
  if (wrapper != nullptr) {
    return wrapper->is_return_nullable();
  }

  return remap != nullptr && remap->_return_nullable;
}

bool
is_parameter_nullable(FunctionRemap *remap, size_t index) {
  const InterrogateFunctionWrapper *wrapper = get_wrapper_for_remap(remap);
  if (wrapper != nullptr && index < (size_t)wrapper->number_of_parameters()) {
    return wrapper->parameter_is_nullable((int)index);
  }

  return remap != nullptr && index < remap->_parameters.size() && remap->_parameters[index]._nullable;
}

TypeIndex
get_type_index_for_interrogate_type(const InterrogateType &itype) {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();

  if (itype.has_scoped_name()) {
    TypeIndex type_index = idb->lookup_type_by_scoped_name(itype.get_scoped_name());
    if (type_index != 0) {
      return type_index;
    }
  }

  if (itype._cpptype != nullptr) {
    CPPType *resolved = TypeManager::resolve_type(itype._cpptype);
    if (resolved != nullptr) {
      return idb->lookup_type_by_scoped_name(resolved->get_fully_scoped_name());
    }
  }

  return 0;
}

TypeIndex
get_type_index_for_cpp_type(CPPType *type, CPPScope *scope = nullptr) {
  if (type == nullptr) {
    return 0;
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  CPPType *resolved = TypeManager::resolve_type(type, scope);
  if (resolved == nullptr) {
    return 0;
  }

  return idb->lookup_type_by_scoped_name(resolved->get_fully_scoped_name());
}

TypeIndex
unwrap_type_aliases(TypeIndex type_index) {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  std::set<TypeIndex> visited;

  while (type_index != 0 && visited.insert(type_index).second) {
    const InterrogateType &itype = idb->get_type(type_index);
    if (!itype.is_typedef() && !itype.is_const()) {
      break;
    }
    type_index = itype.get_wrapped_type();
  }

  return type_index;
}

bool
is_char_type(TypeIndex type_index) {
  type_index = unwrap_type_aliases(type_index);
  if (type_index == 0) {
    return false;
  }

  const InterrogateType &itype = InterrogateDatabase::get_ptr()->get_type(type_index);
  if (!itype.is_atomic() || itype.get_atomic_token() != AT_char) {
    return false;
  }
  // `unsigned char` and `signed char` share AT_char with plain `char`, but they are
  // not text: in C++ `char *` is a string and `unsigned char *` is a byte buffer.
  // Treating them alike marshalled a binary buffer as UTF-8, which truncates it at the
  // first NUL (GeomVertexArrayDataHandle::copy_data_from took a `string`).
  return !itype.is_unsigned() && !itype.is_signed();
}

string
get_pinvoke_atomic_type(const InterrogateType &itype, bool for_return) {
  if (!itype.is_atomic()) {
    return "IntPtr";
  }

  switch (itype.get_atomic_token()) {
  case AT_void:
    return "void";
  case AT_bool:
    return "bool";
  case AT_float:
    return "float";
  case AT_double:
    return "double";
  case AT_string:
    // As a parameter, the [LibraryImport] source generator marshals the
    // managed string -> UTF-8 -> char const *.  As a return, however, it
    // would also attempt to free the returned pointer (CoTaskMemFree), and
    // the pointer we return is into C++ static/member storage — not a
    // CoTaskMem allocation.  Return as IntPtr so the caller can copy into a
    // managed string with Marshal.PtrToStringUTF8 without freeing.
    return for_return ? "IntPtr" : "string";
  case AT_char:
    if (itype.is_unsigned()) return "byte";
    if (itype.is_signed()) return "sbyte";
    return "byte";
  case AT_int:
    if (itype.is_longlong()) return itype.is_unsigned() ? "ulong" : "long";
    if (itype.is_short()) return itype.is_unsigned() ? "ushort" : "short";
    if (itype.is_long()) return itype.is_unsigned() ? "uint" : "int";
    return itype.is_unsigned() ? "uint" : "int";
  case AT_longlong:
    return itype.is_unsigned() ? "ulong" : "long";
  case AT_null:
    return "IntPtr";
  case AT_istream:
  case AT_ostream:
  case AT_iostream:
    // Stream parameters cross the C ABI as an opaque void* pointing at a
    // streambuf bridge; the managed signature is System.IO.Stream, handled in
    // get_csharp_type() below.
    return "IntPtr";
  case AT_not_atomic:
    break;
  }

  return "int";
}

TypeIndex
get_return_type_for_remap(FunctionRemap *remap) {
  const InterrogateFunctionWrapper *wrapper = get_wrapper_for_remap(remap);
  if (wrapper != nullptr) {
    return wrapper->get_return_type();
  }

  return (remap != nullptr && remap->_return_type != nullptr)
    ? get_type_index_for_cpp_type(remap->_return_type->get_new_type()) : 0;
}

TypeIndex
get_parameter_type_for_remap(FunctionRemap *remap, size_t index) {
  const InterrogateFunctionWrapper *wrapper = get_wrapper_for_remap(remap);
  if (wrapper != nullptr && index < (size_t)wrapper->number_of_parameters()) {
    return wrapper->parameter_get_type((int)index);
  }

  if (remap != nullptr && index < remap->_parameters.size()) {
    return get_type_index_for_cpp_type(remap->_parameters[index]._remap->get_new_type());
  }

  return 0;
}

bool
is_csharp_enum_type(TypeIndex type_index) {
  type_index = unwrap_type_aliases(type_index);
  return type_index != 0 && InterrogateDatabase::get_ptr()->get_type(type_index).is_enum();
}

bool
is_csharp_native_object_type(TypeIndex type_index, const string &type_name) {
  if (is_csharp_primitive_type(type_name)) {
    return false;
  }

  type_index = unwrap_type_aliases(type_index);
  if (type_index == 0) {
    return false;
  }

  const InterrogateType &itype = InterrogateDatabase::get_ptr()->get_type(type_index);
  if (itype.is_enum()) {
    return false;
  }
  if (itype.is_class() || itype.is_struct()) {
    return true;
  }
  if (itype.is_pointer()) {
    TypeIndex inner = unwrap_type_aliases(itype.get_wrapped_type());
    if (inner != 0) {
      const InterrogateType &inner_type = InterrogateDatabase::get_ptr()->get_type(inner);
      return inner_type.is_class() || inner_type.is_struct();
    }
  }

  return false;
}

bool
is_csharp_type_legal(TypeIndex type_index) {
  type_index = unwrap_type_aliases(type_index);
  if (type_index == 0) {
    return false;
  }

  const InterrogateType &itype = InterrogateDatabase::get_ptr()->get_type(type_index);
  if (itype.is_atomic() || itype.is_enum() || itype.is_pointer()) {
    return true;
  }
  if (itype.is_class() || itype.is_struct() || itype.is_union()) {
    return true;
  }
  if (itype.is_wrapped()) {
    return is_csharp_type_legal(itype.get_wrapped_type());
  }

  return false;
}

/**
 * The C++ name of a type, for a skip-report line.  Prefers the true name (what
 * a person would recognise, e.g. "pvector< unsigned char >") over the mangled
 * database name.
 */
string
report_type_name(TypeIndex type_index) {
  if (type_index == 0) {
    return "<unknown>";
  }
  const InterrogateType &itype = InterrogateDatabase::get_ptr()->get_type(type_index);
  if (itype.has_true_name()) {
    return itype.get_true_name();
  }
  if (itype.has_scoped_name()) {
    return itype.get_scoped_name();
  }
  if (itype.has_name()) {
    return itype.get_name();
  }
  return "<unnamed>";
}

bool
is_blocked_pascal_alias(const string &alias_name) {
  return alias_name == "GetType" || alias_name == "ToString" ||
         alias_name == "Equals" || alias_name == "GetHashCode" ||
         alias_name == "Finalize" || alias_name == "MemberwiseClone";
}

bool
is_wrapper_legal_csharp(const InterrogateFunctionWrapper &wrapper) {
  if (wrapper.is_forced_void_return()) {
    return false;
  }
  if (wrapper.is_extension() && !wrapper.is_csharp_extension()) {
    return false;
  }
  if (wrapper.is_explicit_self()) {
    return false;
  }
  if (wrapper.has_return_value() && !is_csharp_type_legal(wrapper.get_return_type())) {
    return false;
  }
  for (int i = 0; i < wrapper.number_of_parameters(); ++i) {
    if (!is_csharp_type_legal(wrapper.parameter_get_type(i))) {
      return false;
    }
  }
  return true;
}

FunctionRemap *
best_legal_method_remap(InterfaceMaker::Function *func) {
  if (func == nullptr) {
    return nullptr;
  }

  FunctionRemap *best = nullptr;

  InterfaceMaker::Function::Remaps::const_iterator ri;
  for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
    FunctionRemap *remap = (*ri);
    if (
        (remap->_flags & FunctionRemap::F_explicit_self) != 0 ||
        remap->_type == FunctionRemap::T_constructor ||
        remap->_type == FunctionRemap::T_destructor) {
      continue;
    }

    if (best == nullptr || remap->_parameters.size() > best->_parameters.size()) {
      best = remap;
    }
  }

  return best;
}

void
ensure_make_seqs(InterfaceMaker &maker, InterfaceMaker::Object *object) {
  if (object == nullptr) {
    return;
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateType &itype = object->_itype;

  if ((int)object->_make_seqs.size() == itype.number_of_make_seqs()) {
    return;
  }

  std::set<string> seen_names;
  for (InterfaceMaker::MakeSeq *make_seq : object->_make_seqs) {
    if (make_seq != nullptr) {
      seen_names.insert(make_seq->_name);
    }
  }

  int num_make_seqs = itype.number_of_make_seqs();
  for (int i = 0; i < num_make_seqs; ++i) {
    MakeSeqIndex make_seq_index = itype.get_make_seq(i);
    const InterrogateMakeSeq &imake_seq = idb->get_make_seq(make_seq_index);

    string class_name = itype.has_scoped_name() ? itype.get_scoped_name() : get_csharp_type_name(itype);
    string wrapper_name = "MakeSeq_" + InterrogateBuilder::clean_identifier(class_name) +
      "_" + imake_seq.get_name();
    if (!seen_names.insert(wrapper_name).second) {
      continue;
    }

    InterfaceMaker::MakeSeq *make_seq = new InterfaceMaker::MakeSeq(wrapper_name, imake_seq);
    make_seq->_length_getter = maker.record_function(itype, imake_seq.get_length_getter());
    make_seq->_element_getter = maker.record_function(itype, imake_seq.get_element_getter());
    object->_make_seqs.push_back(make_seq);
  }
}

string
get_supplemental_destructor_entry_point(const InterrogateType &itype) {
  string name;
  if (itype.has_scoped_name()) {
    name = itype.get_scoped_name();
    for (char &c : name) {
      if (c == ':' || c == '<' || c == '>' || c == ',' || c == ' ') {
        c = '_';
      }
    }
  } else {
    name = get_csharp_type_name(itype);
  }
  return "_inCSDestr_" + name;
}

string
get_supplemental_destructor_name(const InterrogateType &itype) {
  string name;
  if (itype.has_scoped_name()) {
    name = itype.get_scoped_name();
    for (char &c : name) {
      if (c == ':') c = '_';
    }
  } else {
    name = get_csharp_type_name(itype);
  }
  return make_csharp_identifier("Destroy_" + name);
}

string
get_supplemental_unref_destructor_entry_point(const InterrogateType &itype) {
  string name;
  if (itype.has_scoped_name()) {
    name = itype.get_scoped_name();
    for (char &c : name) {
      if (c == ':' || c == '<' || c == '>' || c == ',' || c == ' ') {
        c = '_';
      }
    }
  } else {
    name = get_csharp_type_name(itype);
  }
  return "_inCSUnrefDestr_" + name;
}

string
get_supplemental_unref_destructor_name(const InterrogateType &itype) {
  string name;
  if (itype.has_scoped_name()) {
    name = itype.get_scoped_name();
    for (char &c : name) {
      if (c == ':') c = '_';
    }
  } else {
    name = get_csharp_type_name(itype);
  }
  return make_csharp_identifier("UnrefDestroy_" + name);
}

}  // namespace

/**
 *
 */
InterfaceMakerCSharp::
InterfaceMakerCSharp(InterrogateModuleDef *def) :
  InterfaceMaker(def),
  _dll_name(csharp_dll_name),
  _csharp_interface_cache_valid(false),
  _csharp_interface_cache_type_count(0)
{
  if (_dll_name.empty()) {
    _dll_name = library_name;
  }
  if (_dll_name.empty() && def != nullptr && def->library_name != nullptr) {
    _dll_name = def->library_name;
  }
}

/**
 *
 */
InterfaceMakerCSharp::
~InterfaceMakerCSharp() {
}

/**
 * The C# backend reuses the C wrappers and does not emit C++ prototypes.
 */
void InterfaceMakerCSharp::
write_prototypes(ostream &, ostream *) {
}

/**
 * The C# backend reuses the C wrappers and does not emit C++ functions.
 */
void InterfaceMakerCSharp::
write_functions(ostream &out) {
  std::vector<Object *> destructor_objects;
  std::vector<Object *> collection_objects;

  Objects::iterator oi;
  for (oi = _objects.begin(); oi != _objects.end(); ++oi) {
    TypeIndex obj_tidx = (*oi).first;
    Object *object = (*oi).second;
    if (object == nullptr || should_skip_csharp_type(object->_itype) ||
        !is_current_native_methods_type(obj_tidx, object->_itype)) {
      continue;
    }

    ensure_make_seqs(*this, object);

    if (object->_itype.has_destructor()) {
      InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
      const InterrogateFunction &ifunc = idb->get_function(object->_itype.get_destructor());
      if (ifunc.number_of_c_wrappers() == 0) {
        destructor_objects.push_back(object);
      }
    }

    CollectionFacadeKind facade_kind = get_collection_facade_kind(object->_itype);
    if (facade_kind != CF_none) {
      collection_objects.push_back(object);
    }
  }

  if (destructor_objects.empty() && collection_objects.empty()) {
    return;
  }

  out << "#if __GNUC__ >= 4\n"
      << "#define EXPORT_FUNC extern \"C\" __attribute__((used, visibility(\"default\")))\n"
      << "#elif defined(_MSC_VER) && !defined(LINK_ALL_STATIC) && !defined(STATIC_BUILD)\n"
      << "#define EXPORT_FUNC extern \"C\" __declspec(dllexport)\n"
      << "#else\n"
      << "#define EXPORT_FUNC extern \"C\"\n"
      << "#endif\n\n";

  // Collect all objects that are RefCounted (for unref_delete wrappers).
  std::vector<Object *> refcounted_objects;
  for (oi = _objects.begin(); oi != _objects.end(); ++oi) {
    TypeIndex obj_tidx = (*oi).first;
    Object *object = (*oi).second;
    if (object == nullptr || should_skip_csharp_type(object->_itype) ||
        !is_current_native_methods_type(obj_tidx, object->_itype) ||
        !object->_itype.has_destructor()) {
      continue;
    }
    CPPType *cpptype = object->_itype._cpptype != nullptr
      ? TypeManager::resolve_type(object->_itype._cpptype) : nullptr;
    if (cpptype != nullptr && TypeManager::is_reference_count(cpptype)) {
      refcounted_objects.push_back(object);
    }
  }

  for (Object *object : destructor_objects) {
    CPPType *cpptype = object->_itype._cpptype != nullptr
      ? TypeManager::resolve_type(object->_itype._cpptype) : nullptr;
    string entry_point = get_supplemental_destructor_entry_point(object->_itype);

    out << "EXPORT_FUNC void " << entry_point << "(";
    if (cpptype != nullptr) {
      CPPType *pointer_type = TypeManager::wrap_pointer(cpptype);
      pointer_type->output_instance(out, 0, &parser, false, "", "self");
    } else {
      string cpp_type_name;
      if (object->_itype.has_true_name()) {
        cpp_type_name = object->_itype.get_true_name();
      } else if (object->_itype.has_scoped_name()) {
        cpp_type_name = object->_itype.get_scoped_name();
      } else {
        cpp_type_name = get_class_name(object->_itype);
      }
      out << cpp_type_name << " *self";
    }
    out << ") {\n";
    indent(out, 2) << "delete self;\n";
    out << "}\n\n";
  }

  // Emit unref_delete wrappers for all RefCounted types.
  for (Object *object : refcounted_objects) {
    CPPType *cpptype = TypeManager::resolve_type(object->_itype._cpptype);
    string entry_point = get_supplemental_unref_destructor_entry_point(object->_itype);

    out << "EXPORT_FUNC void " << entry_point << "(";
    CPPType *pointer_type = TypeManager::wrap_pointer(cpptype);
    pointer_type->output_instance(out, 0, &parser, false, "", "self");
    out << ") {\n";
    indent(out, 2) << "if (self != nullptr) {\n";
    indent(out, 4) << "unref_delete(self);\n";
    indent(out, 2) << "}\n";
    out << "}\n\n";
  }

  // Several InterrogateTypes can share the same collection class name
  // (e.g. "vector_int" and its const-qualified variant both derive their
  // helper prefix from the base class name).  Track emitted prefixes so the
  // second registration doesn't produce duplicate `extern "C"` definitions.
  std::set<string> emitted_collection_prefixes;

  for (Object *object : collection_objects) {
    const InterrogateType &itype = object->_itype;
    CollectionFacadeKind facade_kind = get_collection_facade_kind(itype);
    if (facade_kind == CF_none) continue;

    bool is_mutable = (facade_kind == CF_mutable_array);
    string helper_prefix = get_collection_helper_name(itype, "");
    if (!emitted_collection_prefixes.insert(helper_prefix).second) {
      continue;
    }
    string cpp_type;
    if (itype._cpptype != nullptr) {
      cpp_type = itype._cpptype->get_local_name(&parser);
    } else if (itype.has_scoped_name()) {
      cpp_type = InterrogateBuilder::descope(itype.get_scoped_name());
    } else {
      cpp_type = itype.get_name();
    }

    // If interrogate exposed this as a pointer-to-collection type (e.g.
    // "vector_string *" or "ConstPointerToArray<double> const *"), strip the
    // trailing pointer so that the helpers operate on the collection directly.
    // Without this, "new vector_string *()" allocates a vector_string** and
    // the self parameter becomes a double-pointer.
    while (!cpp_type.empty() && cpp_type.back() == '*') {
      cpp_type.pop_back();
      while (!cpp_type.empty() && cpp_type.back() == ' ') {
        cpp_type.pop_back();
      }
    }

    // Build base_cpp_type without a trailing cv-qualifier so that
    // "TYPE const::value_type" (invalid C++) is never produced.
    // cpp_type retains its const for use as function parameter types (e.g.
    // "ConstPointerToArray<double> const *self" is correct and desirable).
    string base_cpp_type = cpp_type;
    if (base_cpp_type.size() >= 5 &&
        base_cpp_type.substr(base_cpp_type.size() - 5) == "const") {
      base_cpp_type = base_cpp_type.substr(0, base_cpp_type.size() - 5);
      while (!base_cpp_type.empty() && base_cpp_type.back() == ' ') {
        base_cpp_type.pop_back();
      }
    }

    // If cpp_type had a trailing const, the type is effectively read-only:
    // non-const member functions (push_back, clear, resize, set_element) cannot
    // be called on it, and "new const T()" returns const T* which can't
    // implicitly convert to void*.  Track this so we can suppress mutable
    // helpers and use base_cpp_type for allocation.
    bool is_const_type = (cpp_type != base_cpp_type);

    string element_type_value = get_collection_element_type(itype, false);
    bool is_string = (element_type_value == "string");
    bool is_primitive = is_csharp_primitive_type(element_type_value);
    bool is_blittable = is_csharp_blittable_type(element_type_value);
    string e_cpp_type = base_cpp_type + "::value_type";

    // Const-qualified types are read-only: suppress mutable helpers.
    if (is_const_type) {
      is_mutable = false;
    }

    // Only if the type really can be default-constructed.  panda3d's
    // ReferenceCountedVector<T> is a vector but every constructor takes a
    // TypeHandle, so `new T()` does not compile; the db records the answer
    // (F_default_constructible) because pass 2 cannot work it out.
    // Use base_cpp_type (no const) for allocation: "new const T()" returns
    // const T* which cannot convert to void*.
    if (itype.is_default_constructible()) {
      out << "EXPORT_FUNC void *" << helper_prefix << "empty_constructor() { return new " << base_cpp_type << "(); }\n";
    }
    out << "EXPORT_FUNC int " << helper_prefix << "size(" << cpp_type << " *self) { return self->size(); }\n";
    // A PointerTo<T> element is a refcounted handle: hand the pointer across as it
    // is (ref'd, so the managed side owns a reference), instead of heap-copying the
    // smart pointer itself.
    string element_true_name;
    detect_collection_facade_kind(itype, element_true_name);
    string element_pointee = collection_element_pointee(element_true_name);
    bool is_handle = !element_pointee.empty();

    out << "EXPORT_FUNC ";
    if (is_string) {
      out << "const char *" << helper_prefix << "get_element(" << cpp_type << " *self, int index) { return (*self)[index].c_str(); }\n";
    } else if (is_primitive) {
      out << e_cpp_type << " " << helper_prefix << "get_element(" << cpp_type << " *self, int index) { return (*self)[index]; }\n";
    } else if (is_handle) {
      out << "void *" << helper_prefix << "get_element(" << cpp_type << " *self, int index) { "
          << element_pointee << " *__p = (*self)[index].p(); if (__p != nullptr) { __p->ref(); } return (void *)__p; }\n";
    } else {
      out << "void *" << helper_prefix << "get_element(" << cpp_type << " *self, int index) { return new " << e_cpp_type << "((*self)[index]); }\n";
    }

    if (is_mutable) {
      out << "EXPORT_FUNC void " << helper_prefix << "set_element(" << cpp_type << " *self, int index, ";
      if (is_string) {
        out << "const char *val) { (*self)[index] = val; }\n";
      } else if (is_primitive) {
        out << e_cpp_type << " val) { (*self)[index] = val; }\n";
      } else if (is_handle) {
        out << "void *val) { (*self)[index] = (" << element_pointee << " *)val; }\n";
      } else {
        out << e_cpp_type << " *val) { (*self)[index] = *val; }\n";
      }
      out << "EXPORT_FUNC void " << helper_prefix << "push_back(" << cpp_type << " *self, ";
      if (is_string) {
        out << "const char *val) { self->push_back(val); }\n";
      } else if (is_handle) {
        out << "void *val) { self->push_back((" << element_pointee << " *)val); }\n";
      } else if (is_primitive) {
        out << e_cpp_type << " val) { self->push_back(val); }\n";
      } else {
        out << e_cpp_type << " *val) { self->push_back(*val); }\n";
      }
      out << "EXPORT_FUNC void " << helper_prefix << "clear(" << cpp_type << " *self) { self->clear(); }\n";
      if (is_blittable) {
        out << "EXPORT_FUNC void " << helper_prefix << "resize(" << cpp_type << " *self, int n) { self->resize(n); }\n";
      }
    }

    // Bulk data helpers for blittable types (zero-copy via pointer)
    if (is_blittable) {
      // ConstPointerToArray::operator[] returns a const reference, so
      // &(*self)[0] is a const pointer.  Use const_cast to obtain a mutable
      // pointer — the C# side wraps this in ReadOnlySpan for const collections,
      // so the cast is safe: no mutation will ever occur through the pointer.
      out << "EXPORT_FUNC " << e_cpp_type << " *" << helper_prefix << "get_data_ptr(" << cpp_type << " *self) { return const_cast<" << e_cpp_type << " *>(&(*self)[0]); }\n";
      out << "EXPORT_FUNC int " << helper_prefix << "get_data_size_bytes(" << cpp_type << " *self) { return (int)(self->size() * sizeof(" << e_cpp_type << ")); }\n";
    }
  }
}

/**
 * Emits the C# output files.  Only runs in the database-only pass
 * (interrogate_csharp, pass 2) which has full cross-library type information.
 * Pass 1 (interrogate --csharp) only emits supplemental native helpers via
 * write_functions and never reaches this path.
 */
void InterfaceMakerCSharp::
write_module_support(ostream &, ostream *, InterrogateModuleDef *def) {
  if (csharp_database_only_pass) {
    write_csharp_files(def != nullptr ? def : _def);
  }
}

void InterfaceMakerCSharp::
write_module(ostream &, ostream *, InterrogateModuleDef *) {
}

/**
 * The generated P/Invoke signatures match the C backend and therefore include
 * the synthetic this parameter.
 */
bool InterfaceMakerCSharp::
synthesize_this_parameter() {
  return true;
}

/**
 * Uses the same wrapper name prefix as the C backend so the hashes resolve to
 * the same exported symbol names.
 */
string InterfaceMakerCSharp::
get_wrapper_prefix() {
  return "_inC";
}

/**
 * Uses the same unique-name prefix as the C backend.
 */
string InterfaceMakerCSharp::
get_unique_prefix() {
  return "c";
}

/**
 * The C backend records the wrapper entries in the database; the C# backend
 * only consumes them.
 */
void InterfaceMakerCSharp::
record_function_wrapper(InterrogateFunction &, FunctionWrapperIndex) {
}

/**
 *
 */
void InterfaceMakerCSharp::
write_csharp_files(InterrogateModuleDef *def) {
  if (def == nullptr) {
    return;
  }

  Filename output_dir = csharp_output_dir;
  if (output_dir.empty()) {
    output_dir = Filename("csharp_out");
  }

  if (!output_dir.exists()) {
    Filename dir_with_slash(output_dir.get_fullpath() + "/");
    dir_with_slash.make_dir();
    if (!output_dir.exists()) {
      nout << "Failed to create " << output_dir << ".\n";
      return;
    }
  } else if (!output_dir.is_directory()) {
    nout << output_dir << " is not a directory.\n";
    return;
  }

  string raw_module;
  if (!module_name.empty()) {
    raw_module = module_name;
  } else if (def->module_name != nullptr) {
    raw_module = def->module_name;
  }
  string cs_namespace = prettify_namespace(raw_module);
  _current_module_name = raw_module;
  _current_library_name = _dll_name;
  if (!library_name.empty()) {
    _current_library_name = library_name;
  } else if (def->library_name != nullptr) {
    _current_library_name = def->library_name;
  }

  string dir = output_dir.to_os_generic();
  _written_enums.clear();

  Objects::iterator oi;
  for (oi = _objects.begin(); oi != _objects.end(); ++oi) {
    ensure_make_seqs(*this, (*oi).second);
  }

  // Pre-mark every database that was already loaded for this module's owned
  // types.  load_all_search_dir_databases() uses request_external_database()
  // for de-duplication, but command-line databases loaded via idb->read_file()
  // in main() are NOT tracked there.  If we don't mark them here,
  // load_all_search_dir_databases() would re-load them, creating duplicate type
  // stubs.  Re-loading a stub database AFTER its canonical database has been
  // merged into it resets the merged data (e.g. TextEncoder at TypeIndex N
  // goes back to 0 methods), breaking secondary-base resolution.
  {
    InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
    for (TypeIndex tidx : csharp_owned_type_indices) {
      const InterrogateType &itype = idb->get_type(tidx);
      if (itype.has_library_name()) {
        string lib = itype.get_library_name();
        if (!lib.empty()) {
          _loaded_external_databases.insert(lib + ".in");
        }
      }
    }
  }

  // Before loading search-dir databases, take a snapshot of every type's
  // module assignment into csharp_type_module_map.  Search-dir merges can
  // change _def (and thus get_library_name()) for stub types: e.g. MemoryBase
  // is first loaded from p3dtoolbase (panda3d.core) but a later merge from
  // p3egg.in sets _def to p3egg, making get_type_module_name() return
  // "panda3d.egg" in the core module run.  The snapshot preserves the
  // pre-merge attribution so code generation uses the correct namespace.
  {
    InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
    int n = idb->get_num_all_types();
    for (int t = 0; t < n; ++t) {
      TypeIndex idx = idb->get_all_type(t);
      if (csharp_type_module_map.count(idx)) {
        continue;  // already attributed (from command-line range tracking
                   // or the second-pass ownership determination)
      }
      const InterrogateType &itype = idb->get_type(idx);
      if (itype.has_library_name()) {
        string lib = itype.get_library_name();
        auto it = csharp_library_to_module.find(lib);
        if (it != csharp_library_to_module.end()) {
          csharp_type_module_map[idx] = it->second;
        }
      }
    }
  }

  // Load all databases from the search directories before any code generation.
  // This ensures:
  //   1. Interface-name collision detection (e.g. ISocketStream vs SocketStream)
  //      works regardless of build order.
  //   2. Cross-module secondary-base types (e.g. Namable for EggNamedObject)
  //      have their method lists available for record_secondary_base_members.
  // New types added here are in the global database but NOT in _objects, so
  // they will not generate extra .cs files.
  load_all_search_dir_databases();

  // After search-dir loading, search-dir merges may have changed the _def
  // (and thus get_library_name()) of collection facade types.  Update
  // csharp_type_module_map for facades using the post-merge library attribution
  // so that they are generated by exactly one module — the one that canonically
  // defines the collection element type's library.
  {
    InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
    int n = idb->get_num_all_types();
    for (int t = 0; t < n; ++t) {
      TypeIndex idx = idb->get_all_type(t);
      const InterrogateType &itype = idb->get_type(idx);
      if (!is_collection_facade_type(itype)) {
        continue;
      }
      // Attribute the facade to its canonical owner library (the same one
      // get_collection_helper_name uses) so it is generated by exactly one
      // module, which declares the entry point it references.
      string lib = get_collection_canonical_library(itype);
      if (lib.empty() && itype.has_library_name()) {
        lib = itype.get_library_name();
      }
      if (!lib.empty()) {
        auto it = csharp_library_to_module.find(lib);
        if (it != csharp_library_to_module.end()) {
          // Overwrite with the post-merge canonical module
          csharp_type_module_map[idx] = it->second;
        }
      }
    }
  }

  record_secondary_base_members();
  write_support_file(dir, cs_namespace);
  write_native_methods_file(dir, cs_namespace);
  write_enum_files(dir, cs_namespace);
  write_class_files(dir, cs_namespace);
  write_globals_file(dir, cs_namespace, def);
}

/**
 * Marks the given database file as already loaded so that
 * load_all_search_dir_databases() will not re-load it.  Call this for every
 * command-line .in file before invoking load_all_search_dir_databases().
 */
void InterfaceMakerCSharp::
mark_database_loaded(const Filename &database_file) {
  _loaded_external_databases.insert(database_file.get_basename());
}

/**
 * Loads all interrogate databases found in the configured search directories.
 * This is called once at the beginning of write_csharp_files() so that
 * cross-module type information is available for:
 *   - Interface-name collision detection in get_interface_name().
 *   - Secondary-base-type resolution in get_secondary_base_types() /
 *     get_interface_base_list().
 *   - NativeMethods generation for cross-module secondary bases.
 *
 * New types added here land only in the global database, NOT in _objects,
 * so no extra .cs files are generated.
 */
void InterfaceMakerCSharp::
load_all_search_dir_databases() {
  std::function<void(const Filename &)> scan_dir = [&](const Filename &dir) {
    vector_string entries;
    if (!dir.scan_directory(entries)) return;
    for (const string &entry : entries) {
      if (entry == "." || entry == "..") continue;
      Filename child(dir, entry);
      if (child.is_directory()) {
        scan_dir(child);
      } else if (entry.size() > 3 && entry.substr(entry.size() - 3) == ".in") {
        request_external_database(child);
      }
    }
  };

  for (const Filename &dir : database_search_dirs) {
    scan_dir(dir);
  }
}

/**
 *
 */
void InterfaceMakerCSharp::
record_secondary_base_members() {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();

  Objects::const_iterator oi;
  for (oi = _objects.begin(); oi != _objects.end(); ++oi) {
    Object *object = (*oi).second;
    if (object == nullptr || should_skip_csharp_type(object->_itype)) {
      continue;
    }

    std::vector<const InterrogateType *> secondary_base_types;
    get_secondary_base_types(object->_itype, secondary_base_types);
    for (const InterrogateType *base_type : secondary_base_types) {
      if (base_type == nullptr) {
        continue;
      }

      // Record secondary base members regardless of library/module.
      // Cross-module secondary bases still need their remaps/wrappers
      // set up correctly when emitted in write_interface() and
      // write_proxy_class().

      for (int mi = 0; mi < base_type->number_of_methods(); ++mi) {
        record_function(*base_type, base_type->get_method(mi));
      }
      for (int ci = 0; ci < base_type->number_of_casts(); ++ci) {
        record_function(*base_type, base_type->get_cast(ci));
      }
      for (int ei = 0; ei < base_type->number_of_elements(); ++ei) {
        const InterrogateElement &ielement = idb->get_element(base_type->get_element(ei));
        if (ielement.has_getter()) {
          record_function(*base_type, ielement.get_getter());
        }
        if (ielement.has_setter()) {
          record_function(*base_type, ielement.get_setter());
        }
        // A property's supporting accessors need P/Invokes too, and only the getter
        // and setter were ever registered.  MAKE_SEQ_PROPERTY's length function is
        // not PUBLISHED in its own right (InputDevice::get_num_axes is not), so it
        // had no declaration to call -- while get_axis, being the element getter,
        // did.  Same for MAKE_PROPERTY2's has_xxx().
        if (ielement.get_length_function() != 0) {
          record_function(*base_type, ielement.get_length_function());
        }
        if (ielement.has_has_function()) {
          record_function(*base_type, ielement.get_has_function());
        }
        if (ielement.has_getkey_function()) {
          record_function(*base_type, ielement.get_getkey_function());
        }
        if (ielement.has_clear_function()) {
          record_function(*base_type, ielement.get_clear_function());
        }
      }
    }
  }
}

/**
 *
 */
void InterfaceMakerCSharp::
write_support_file(const string &, const string &) {
}

/**
 *
 */
void InterfaceMakerCSharp::
write_native_methods_file(const string &dir, const string &cs_namespace) {
  // NativeMethods_<module>.cs is written once per module by pass 2
  // (interrogate_csharp, csharp_database_only_pass = true).  Pass 1
  // (interrogate --csharp) never reaches this function because
  // write_module_support() only calls write_csharp_files() when
  // csharp_database_only_pass is true, so there is no duplication risk.
  string safe_lib = make_csharp_identifier(library_name.empty() ? _dll_name : library_name);
  string filename = "NativeMethods_" + safe_lib + ".cs";

  std::ofstream out;
  if (!open_output_file(dir, filename, out)) {
    return;
  }

  out << "using System;\n"
      << "using System.Collections.Generic;\n"
      << "using System.Runtime.InteropServices;\n"
      << "using Interrogate;\n";
  emit_peer_namespace_usings(out, cs_namespace, dir);
  out << "\n"
      << "namespace " << cs_namespace << " {\n"
      << "  internal static partial class NativeMethods {\n";

  std::set<string> emitted_pinvoke_names;

  // Emit DllImport declarations for every function recorded for this module,
  // including secondary-base functions (e.g. Namable methods injected into
  // EggNamedObject).  Secondary-base functions are owned by another module
  // (so is_current_native_methods_type returns false for them), but the
  // class file that consumes them lives in THIS module's namespace and
  // therefore resolves NativeMethods.xxx against THIS module's partial class.
  // Each module has its own namespace, so re-declaring the same P/Invoke in
  // two NativeMethods classes is harmless.
  FunctionsByIndex::iterator fi;
  for (fi = _functions.begin(); fi != _functions.end(); ++fi) {
    Function *func = (*fi).second;
    if (func == nullptr) {
      continue;
    }
    Function::Remaps::const_iterator ri;
    bool wrote_remap = false;
    for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
      FunctionRemap *remap = (*ri);
      if ((remap->_flags & FunctionRemap::F_explicit_self) ||
          !is_remap_legal_csharp(remap)) {
        continue;
      }

      string friendly_name = get_pinvoke_name(func, remap);
      if (emitted_pinvoke_names.insert(friendly_name).second) {
        write_dllimport(out, remap, friendly_name);
        out << "\n";
        wrote_remap = true;
      }
    }

    // Fall back to wrapper-based DllImport if no remap produced a legal entry.
    // This covers cross-module secondary-base functions whose FunctionRemap
    // objects have null CPPType* pointers (because the C++ headers weren't
    // parsed in pass 2) but whose InterrogateFunctionWrapper entries are fully
    // serialized and thus legal.
    if (!wrote_remap) {
      int num_wrappers = func->_ifunc.number_of_c_wrappers();
      for (int wi = 0; wi < num_wrappers; ++wi) {
        FunctionWrapperIndex wrapper_index = func->_ifunc.get_c_wrapper(wi);
        if (wrapper_index == 0) {
          continue;
        }

        const InterrogateFunctionWrapper &wrapper = InterrogateDatabase::get_ptr()->get_wrapper(wrapper_index);
        if (!is_wrapper_legal_csharp(wrapper)) {
          continue;
        }

        string friendly_name = get_pinvoke_name(func->_ifunc, wrapper);
        if (emitted_pinvoke_names.insert(friendly_name).second) {
          write_dllimport(out, func->_ifunc, wrapper, friendly_name);
          out << "\n";
        }
      }
    }
  }

  Objects::iterator oi;
  for (oi = _objects.begin(); oi != _objects.end(); ++oi) {
    Object *object = (*oi).second;
    if (object == nullptr || should_skip_csharp_type(object->_itype)) {
      continue;
    }

    const InterrogateType &itype = object->_itype;
    string class_name = get_class_name(itype);

    if (itype.has_destructor()) {
      string destructor_name = get_destructor_wrapper_name(object);
      if (!destructor_name.empty()) {
        out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"";

        InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
        const InterrogateFunction &ifunc = idb->get_function(itype.get_destructor());
        if (ifunc.number_of_c_wrappers() != 0) {
          const InterrogateFunctionWrapper &wrapper = idb->get_wrapper(ifunc.get_c_wrapper(0));
          out << "_inC" << wrapper.get_unique_name();
        } else {
          out << get_supplemental_destructor_entry_point(itype);
        }

        out << "\")]\n";
        out << "    internal static partial void " << destructor_name
            << "(IntPtr self);\n\n";
      }

      // For RefCounted types, also emit an unref_delete P/Invoke.
      TypeIndex type_index = get_type_index_for_interrogate_type(itype);
      if (type_index != 0 && is_type_refcounted(type_index)) {
        string unref_name = get_supplemental_unref_destructor_name(itype);
        out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \""
            << get_supplemental_unref_destructor_entry_point(itype) << "\")]\n";
        out << "    internal static partial void " << unref_name
            << "(IntPtr self);\n\n";
      }
    }

  }

  // Mirror the dedup used by the C++ helper emission above — const variants
  // of the same collection share the helper prefix and must only appear in
  // NativeMethods once.
  std::set<string> emitted_pinvoke_prefixes;

  for (oi = _objects.begin(); oi != _objects.end(); ++oi) {
    Object *object = (*oi).second;
    if (object == nullptr || should_skip_csharp_type(object->_itype)) {
      continue;
    }
    const InterrogateType &itype = object->_itype;
    CollectionFacadeKind facade_kind = get_collection_facade_kind(itype);
    if (facade_kind == CF_none) continue;

    bool is_mutable = (facade_kind == CF_mutable_array);
    string helper_prefix = get_collection_helper_name(itype, "");
    if (!emitted_pinvoke_prefixes.insert(helper_prefix).second) {
      continue;
    }
    string element_type_value = get_collection_element_type(itype, false);
    bool is_string = (element_type_value == "string");
    bool is_primitive = is_csharp_primitive_type(element_type_value);
    bool is_blittable = is_csharp_blittable_type(element_type_value);

    // For string elements: the return-side pinvoke is IntPtr (not string),
    // because [LibraryImport] + StringMarshalling.Utf8 on a string return
    // calls CoTaskMemFree on the returned pointer, but the native helper
    // returns a pointer into C++ storage.  Parameters as string are fine —
    // managed-to-native string marshalling just copies bytes.
    string cs_ret_type = is_string ? "IntPtr" : (is_primitive ? element_type_value : "IntPtr");
    string cs_param_type = is_string ? "string" : (is_primitive ? element_type_value : "IntPtr");

    // Declared only when the native side emits it — see the matching guard on
    // the helper itself.
    if (itype.is_default_constructible()) {
      out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"" << helper_prefix << "empty_constructor\")]\n";
      out << "    internal static partial IntPtr " << helper_prefix << "empty_constructor();\n\n";
    }

    out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"" << helper_prefix << "size\")]\n";
    out << "    internal static partial int " << helper_prefix << "size(IntPtr self);\n\n";

    out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"" << helper_prefix << "get_element\")]\n";
    out << "    internal static partial " << cs_ret_type << " " << helper_prefix << "get_element(IntPtr self, int index);\n\n";

    if (is_mutable) {
      out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"" << helper_prefix << "set_element\"";
      if (is_string) out << ", StringMarshalling = StringMarshalling.Utf8";
      out << ")]\n";
      out << "    internal static partial void " << helper_prefix << "set_element(IntPtr self, int index, " << cs_param_type << " val);\n\n";

      out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"" << helper_prefix << "push_back\"";
      if (is_string) out << ", StringMarshalling = StringMarshalling.Utf8";
      out << ")]\n";
      out << "    internal static partial void " << helper_prefix << "push_back(IntPtr self, " << cs_param_type << " val);\n\n";

      out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"" << helper_prefix << "clear\")]\n";
      out << "    internal static partial void " << helper_prefix << "clear(IntPtr self);\n\n";

      if (is_blittable) {
        out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"" << helper_prefix << "resize\")]\n";
        out << "    internal static partial void " << helper_prefix << "resize(IntPtr self, int n);\n\n";
      }
    }

    // Bulk data P/Invoke for blittable types
    if (is_blittable) {
      out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"" << helper_prefix << "get_data_ptr\")]\n";
      out << "    internal static partial IntPtr " << helper_prefix << "get_data_ptr(IntPtr self);\n\n";

      out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"" << helper_prefix << "get_data_size_bytes\")]\n";
      out << "    internal static partial int " << helper_prefix << "get_data_size_bytes(IntPtr self);\n\n";
    }
  }

  // Global (non-member) functions are emitted by write_globals_file() which
  // also calls NativeMethods.xxx(...).  Emit matching DllImport declarations
  // for every global function whose FunctionIndex is tracked in _functions.
  {
    InterrogateDatabase *idb_g = InterrogateDatabase::get_ptr();
    int num_global_functions = idb_g->get_num_global_functions();
    for (int i = 0; i < num_global_functions; ++i) {
      FunctionIndex func_index = idb_g->get_global_function(i);
      FunctionsByIndex::const_iterator gfi = _functions.find(func_index);
      if (gfi == _functions.end()) {
        continue;
      }
      Function *func = gfi->second;
      if (func == nullptr) {
        continue;
      }
      bool wrote_remap = false;
      for (auto ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
        FunctionRemap *remap = *ri;
        if ((remap->_flags & FunctionRemap::F_explicit_self) ||
            !is_remap_legal_csharp(remap)) {
          continue;
        }
        string friendly_name = get_pinvoke_name(func, remap);
        if (emitted_pinvoke_names.insert(friendly_name).second) {
          write_dllimport(out, remap, friendly_name);
          out << "\n";
          wrote_remap = true;
        }
      }
      if (!wrote_remap) {
        int num_wrappers = func->_ifunc.number_of_c_wrappers();
        for (int wi = 0; wi < num_wrappers; ++wi) {
          FunctionWrapperIndex wrapper_index = func->_ifunc.get_c_wrapper(wi);
          if (wrapper_index == 0) continue;
          const InterrogateFunctionWrapper &wrapper = idb_g->get_wrapper(wrapper_index);
          if (!is_wrapper_legal_csharp(wrapper)) continue;
          string friendly_name = get_pinvoke_name(func->_ifunc, wrapper);
          if (emitted_pinvoke_names.insert(friendly_name).second) {
            write_dllimport(out, func->_ifunc, wrapper, friendly_name);
            out << "\n";
          }
        }
      }
    }
  }

  out << "  }\n"
      << "}\n";
}

/**
 *
 */
void InterfaceMakerCSharp::
write_enum_files(const string &dir, const string &cs_namespace) {
  if (csharp_database_only_pass) {
    InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
    int num_types = idb->get_num_all_types();
    for (int i = 0; i < num_types; ++i) {
      TypeIndex type_index = idb->get_all_type(i);
      if (type_index == 0) {
        continue;
      }

      const InterrogateType &itype = idb->get_type(type_index);
      if (!itype.is_enum() || should_skip_csharp_type(itype) ||
          !is_current_native_methods_type(itype)) {
        continue;
      }

      string enum_name = get_simple_class_name(itype);
      if (!_written_enums.insert(enum_name).second) {
        continue;
      }

      std::ofstream out;
      if (!open_output_file(dir, enum_name + ".cs", out)) {
        continue;
      }

      out << "namespace " << cs_namespace << " {\n";
      int wrappers = open_nesting_wrappers(out, itype);
      write_enum_type(out, itype);
      close_nesting_wrappers(out, wrappers);
      out << "}\n";
    }
    return;
  }

  Objects::iterator oi;
  for (oi = _objects.begin(); oi != _objects.end(); ++oi) {
    TypeIndex obj_tidx = (*oi).first;
    Object *object = (*oi).second;
    const InterrogateType &itype = object->_itype;
    if (!itype.is_enum() || !is_current_native_methods_type(obj_tidx, itype)) {
      continue;
    }

    string enum_name = get_simple_class_name(itype);
    if (!_written_enums.insert(enum_name).second) {
      continue;
    }

    Filename existing(dir + "/" + enum_name + ".cs");
    if (existing.exists()) {
      continue;
    }

    std::ofstream out;
    if (!open_output_file(dir, enum_name + ".cs", out)) {
      continue;
    }

    out << "namespace " << cs_namespace << " {\n";
    int wrappers = open_nesting_wrappers(out, itype);
    write_enum_type(out, itype);
    close_nesting_wrappers(out, wrappers);
    out << "}\n";
  }
}

/**
 *
 */
void InterfaceMakerCSharp::
write_enum_type(ostream &out, const InterrogateType &itype) {
  string enum_name = get_simple_class_name(itype);

  out << "  public enum " << enum_name << " : int {\n";

  // The interrogate database records each enum value under multiple spellings
  // (e.g. both LEFT_X and left_x).  Fold every spelling to one PascalCase
  // member and de-duplicate: case-variants collapse to a single idiomatic
  // C# identifier.  A repeated name that maps to a *different* numeric value
  // (which case-variants never do) is disambiguated rather than dropped.
  std::vector<std::pair<string, int> > members;
  std::map<string, int> seen;
  for (int i = 0; i < itype.number_of_enum_values(); ++i) {
    string ident = make_csharp_enum_member(itype.get_enum_value_name(i));
    int value = itype.get_enum_value(i);
    std::map<string, int>::iterator it = seen.find(ident);
    if (it != seen.end()) {
      if (it->second == value) {
        continue;
      }
      ident += "_" + std::to_string(value);
      if (seen.count(ident)) {
        continue;
      }
    }
    seen[ident] = value;
    members.push_back(std::make_pair(ident, value));
  }

  for (size_t i = 0; i < members.size(); ++i) {
    out << "    " << members[i].first << " = " << members[i].second;
    if (i + 1 < members.size()) {
      out << ',';
    }
    out << "\n";
  }
  out << "  }\n";
}

/**
 *
 */
void InterfaceMakerCSharp::
write_class_files(const string &dir, const string &cs_namespace) {
  Objects::iterator oi;
  for (oi = _objects.begin(); oi != _objects.end(); ++oi) {
    TypeIndex obj_tidx = (*oi).first;
    Object *object = (*oi).second;
    const InterrogateType &itype = object->_itype;
    if (is_empty_pointer_facade_type(itype) ||
        (!is_collection_facade_type(itype) && !itype.is_class() && !itype.is_struct()) || should_skip_csharp_type(itype) ||
        !is_current_native_methods_type(obj_tidx, itype)) {
      continue;
    }

    write_class_file(dir, cs_namespace, object);
  }


}

/**
 *
 */
void InterfaceMakerCSharp::
write_class_file(const string &dir, const string &cs_namespace, Object *object) {
  if (object == nullptr || is_empty_pointer_facade_type(object->_itype)) {
    return;
  }

  string class_name = get_class_name(object->_itype);
  bool is_collection_facade = get_collection_facade_kind(object->_itype) != CF_none;

  if (is_collection_facade && csharp_database_only_pass) {
    // Collection facades (vector<T> typedefs) carry neither F_global nor
    // F_nested, so they appear in MULTIPLE modules' _objects.  The same file
    // (e.g. vector_uchar.cs) would be generated — and overwritten — by each
    // module that runs.  Use first-come-first-served: if the file already
    // exists it was generated by an earlier module (panda3d.core always runs
    // before other modules due to cmake dependency order).  Accept that
    // version and skip re-generation to keep the namespace stable.
    Filename existing = make_output_filename(dir, class_name + ".cs");
    if (existing.exists()) {
      return;
    }
  }

  std::ofstream out;
  if (!open_output_file(dir, class_name + ".cs", out)) {
    return;
  }

  out << "using System;\n"
      << "using System.Collections;\n"
      << "using System.Collections.Generic;\n"
      << "using System.Runtime.InteropServices;\n"
      << "using Interrogate;\n";

  emit_peer_namespace_usings(out, cs_namespace, dir);
  out << "\n"
      << "namespace " << cs_namespace << " {\n";

  if (is_collection_facade) {
    write_collection_adapter_class(out, object);
    out << "}\n";
    return;
  }

  // A type nested in a C++ class is emitted as a real nested C# type by
  // reopening its enclosing class(es) as partial classes.
  int wrappers = open_nesting_wrappers(out, object->_itype);
  if (uses_csharp_interface(object->_itype)) {
    write_interface(out, object);
    out << "\n";
  }
  write_proxy_class(out, cs_namespace, object);
  close_nesting_wrappers(out, wrappers);
  out << "}\n";
}

/**
 *
 */
void InterfaceMakerCSharp::
write_interface(ostream &out, Object *object) {
  string interface_name = get_simple_interface_name(object->_itype);
  bool is_collection_facade = is_collection_facade_type(object->_itype);

  // When the class surfaces as a sequence, the interface declares the same
  // public indexer and suppresses op_index (which the class no longer emits).
  string indexer_type;
  {
    Function *lf = nullptr, *ef = nullptr;
    const InterrogateFunctionWrapper *lw = nullptr, *ew = nullptr;
    TypeIndex ei = 0;
    if (!is_collection_facade) {
      find_sequence_indexer(object, lf, lw, ef, ew, ei, indexer_type);
    }
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateType &itype = object->_itype;

  int num_elements = itype.number_of_elements();

  out << "  " << (is_collection_facade ? "internal" : "public") << " interface " << interface_name
      << get_interface_base_list(itype) << " {\n";

  string base_list = get_interface_base_list(itype);
  if (!base_list.empty() && base_list.find("INativeObject") != string::npos) {
    indent(out, 4) << "new IntPtr NativeHandle { get; }\n";
  }

  std::set<string> method_signatures;
  auto reserve_property_names = [&](const InterrogateType &source_type) {
    for (int i = 0; i < source_type.number_of_elements(); ++i) {
      const InterrogateElement &ielement = idb->get_element(source_type.get_element(i));
      method_signatures.insert("N:" +
        to_pascal_case(make_csharp_identifier(ielement.get_name())));
    }
  };
  auto reserve_method_signatures = [&](Function *method) {
    if (method == nullptr) {
      return;
    }

    string reserved_name = make_csharp_identifier(method->_ifunc.get_name());
    Function::Remaps::const_iterator ri;
    for (ri = method->_remaps.begin(); ri != method->_remaps.end(); ++ri) {
      FunctionRemap *remap = (*ri);
      if ((remap->_flags & FunctionRemap::F_explicit_self) != 0 ||
          remap->_type == FunctionRemap::T_constructor ||
          remap->_type == FunctionRemap::T_destructor ||
          !is_remap_legal_csharp(remap) || !remap->_has_this) {
        continue;
      }

      std::vector<string> reserved_param_types;
      for (size_t i = 1; i < remap->_parameters.size(); ++i) {
        reserved_param_types.push_back(
          get_csharp_signature_type_for_wrapper(remap->_parameters[i]._remap, false));
      }

      method_signatures.insert("R:" +
        build_signature_key(reserved_name, reserved_param_types, false));
    }
  };

  reserve_property_names(itype);
  for (Function *method : object->_methods) {
    reserve_method_signatures(method);
  }

  // Also reserve signatures from secondary bases to avoid conflicts
  std::vector<const InterrogateType *> secondary_base_types;
  get_secondary_base_types(itype, secondary_base_types);
  for (const InterrogateType *base_type : secondary_base_types) {
    if (base_type == nullptr) {
      continue;
    }
    reserve_property_names(*base_type);
    for (int mi = 0; mi < base_type->number_of_methods(); ++mi) {
      reserve_method_signatures(record_function(*base_type, base_type->get_method(mi)));
    }
    for (int ci = 0; ci < base_type->number_of_casts(); ++ci) {
      reserve_method_signatures(record_function(*base_type, base_type->get_cast(ci)));
    }
  }

  Functions::const_iterator fi;
  for (fi = object->_methods.begin(); fi != object->_methods.end(); ++fi) {
    // operator[] is superseded by the class's public indexer, so drop it from
    // the interface too — otherwise the class fails to implement the member.
    if (!indexer_type.empty() && (*fi)->_ifunc.has_name() &&
        make_csharp_identifier((*fi)->_ifunc.get_name()) == "op_index") {
      continue;
    }
    write_method(out, (*fi), object, 4, true, &method_signatures);
  }

  // Emit secondary base methods in the interface
  for (const InterrogateType *base_type : secondary_base_types) {
    if (base_type == nullptr) {
      continue;
    }
    for (int mi = 0; mi < base_type->number_of_methods(); ++mi) {
      Function *base_method = record_function(*base_type, base_type->get_method(mi));
      write_method(out, base_method, object, 4, true, &method_signatures);
    }
    for (int ci = 0; ci < base_type->number_of_casts(); ++ci) {
      Function *base_cast = record_function(*base_type, base_type->get_cast(ci));
      write_method(out, base_cast, object, 4, true, &method_signatures);
    }
  }

  write_operator_aliases(out, object, 4, &method_signatures, true);

  std::set<string> emitted_prop_names;
  // Emit properties from secondary bases in the interface
  for (const InterrogateType *base_type : secondary_base_types) {
    if (base_type == nullptr) {
      continue;
    }
    for (int ei = 0; ei < base_type->number_of_elements(); ++ei) {
      const InterrogateElement &ielement = idb->get_element(base_type->get_element(ei));
      string pn = to_pascal_case(make_csharp_identifier(ielement.get_name()));
      if (emitted_prop_names.insert(pn).second) {
        Property property(ielement);
        if (ielement.has_getter()) {
          FunctionIndex func_index = ielement.get_getter();
          FunctionsByIndex::const_iterator fi2 = _functions.find(func_index);
          if (fi2 != _functions.end()) {
            Function *func = (*fi2).second;
            property._has_this |= func->_has_this;
            Function::Remaps::const_iterator ri;
            for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
              FunctionRemap *remap = (*ri);
              if ((remap->_flags & FunctionRemap::F_explicit_self) == 0) {
                property._getter_remaps.push_back(remap);
              }
            }
          }
        }
        if (ielement.has_setter()) {
          FunctionIndex func_index = ielement.get_setter();
          FunctionsByIndex::const_iterator fi2 = _functions.find(func_index);
          if (fi2 != _functions.end()) {
            Function *func = (*fi2).second;
            property._has_this |= func->_has_this;
            Function::Remaps::const_iterator ri;
            for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
              FunctionRemap *remap = (*ri);
              if ((remap->_flags & FunctionRemap::F_explicit_self) == 0) {
                property._setter_remaps.push_back(remap);
              }
            }
          }
        }
        write_property(out, &property, object, 4, true);
      }
    }
  }
  for (int ei = 0; ei < num_elements; ++ei) {
    const InterrogateElement &ielement = idb->get_element(itype.get_element(ei));
    Property property(ielement);

    if (ielement.has_getter()) {
      FunctionIndex func_index = ielement.get_getter();
      FunctionsByIndex::const_iterator fi2 = _functions.find(func_index);
      if (fi2 != _functions.end()) {
        Function *func = (*fi2).second;
        property._has_this |= func->_has_this;
        Function::Remaps::const_iterator ri;
        for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
          FunctionRemap *remap = (*ri);
          if (
              (remap->_flags & FunctionRemap::F_explicit_self) == 0) {
            property._getter_remaps.push_back(remap);
          }
        }
      }
    }

    if (ielement.has_setter()) {
      FunctionIndex func_index = ielement.get_setter();
      FunctionsByIndex::const_iterator fi2 = _functions.find(func_index);
      if (fi2 != _functions.end()) {
        Function *func = (*fi2).second;
        property._has_this |= func->_has_this;
        Function::Remaps::const_iterator ri;
        for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
          FunctionRemap *remap = (*ri);
          if (
              (remap->_flags & FunctionRemap::F_explicit_self) == 0) {
            property._setter_remaps.push_back(remap);
          }
        }
      }
    }

    string pn = to_pascal_case(make_csharp_identifier(ielement.get_name()));
    if (emitted_prop_names.insert(pn).second) {
      if (property._getter_remaps.empty() && property._setter_remaps.empty()) {
        // Pass 2 (binary .in) can't rebuild FunctionRemaps for interrogate-synthesized data-member
        // getters/setters (no CPPInstance available). Fall back to the .in C-wrapper records, exactly
        // as methods do, so public data members (incl. enum-typed) still emit as properties.
        write_property_from_wrapper(out, ielement, object, 4, true);
      } else {
        write_property(out, &property, object, 4, true);
      }
    }
  }

  out << "  }\n";
}

/**
 * Emits the internal native-backed collection adapter for PointerToArray-style
 * facade types.  These types are implementation details; the public API should
 * surface stdlib collection interfaces instead.
 */
void InterfaceMakerCSharp::
write_collection_adapter_class(ostream &out, Object *object) {
  const InterrogateType &itype = object->_itype;
  string class_name = get_class_name(itype);
  string element_type = get_collection_element_type(itype, true);
  string element_value_type = get_collection_element_type(itype, false);
  string destructor_name = get_destructor_wrapper_name(object);
  CollectionFacadeKind facade_kind = get_collection_facade_kind(itype);
  bool is_mutable = facade_kind == CF_mutable_array;

  MakeSeq *indexer_make_seq = nullptr;
  Function *indexer_length_func = nullptr;
  Function *indexer_element_func = nullptr;
  FunctionRemap *indexer_length_remap = nullptr;
  FunctionRemap *indexer_element_remap = nullptr;
  for (MakeSeq *make_seq : object->_make_seqs) {
    if (make_seq == nullptr || make_seq->_length_getter == nullptr ||
        make_seq->_element_getter == nullptr) {
      continue;
    }

    FunctionRemap *length_remap = nullptr;
    FunctionRemap *element_remap = nullptr;
    size_t min_lp = 999;
    for (auto ri = make_seq->_length_getter->_remaps.begin();
         ri != make_seq->_length_getter->_remaps.end(); ++ri) {
      FunctionRemap *r = *ri;
      if (r != nullptr && is_remap_legal_csharp(r) && r->_parameters.size() < min_lp) {
        min_lp = r->_parameters.size();
        length_remap = r;
      }
    }
    size_t min_ep = 999;
    for (auto ri = make_seq->_element_getter->_remaps.begin();
         ri != make_seq->_element_getter->_remaps.end(); ++ri) {
      FunctionRemap *r = *ri;
      if (r == nullptr || !is_remap_legal_csharp(r) || r->_void_return) {
        continue;
      }
      size_t eself = r->_has_this ? 1 : 0;
      size_t eextra = r->_parameters.size() - eself;
      if (eextra >= 1 && r->_parameters.size() < min_ep) {
        min_ep = r->_parameters.size();
        element_remap = r;
      }
    }

    if (length_remap != nullptr && element_remap != nullptr) {
      indexer_make_seq = make_seq;
      indexer_length_func = make_seq->_length_getter;
      indexer_element_func = make_seq->_element_getter;
      indexer_length_remap = length_remap;
      indexer_element_remap = element_remap;
      break;
    }
  }

  if (indexer_length_remap == nullptr || indexer_element_remap == nullptr) {
    for (Function *method : object->_methods) {
      if (method == nullptr || !method->_ifunc.has_name()) {
        continue;
      }
      string name = method->_ifunc.get_name();
      if (name == "size" && indexer_length_remap == nullptr) {
        indexer_make_seq = nullptr;
        indexer_length_func = method;
        indexer_length_remap = best_legal_method_remap(method);
      } else if (name == "get_element" && indexer_element_remap == nullptr) {
        indexer_make_seq = nullptr;
        indexer_element_func = method;
        indexer_element_remap = best_legal_method_remap(method);
      }
    }
  }

  Function *set_element_func = nullptr;
  FunctionRemap *set_element_remap = nullptr;
  const InterrogateFunctionWrapper *set_element_wrapper = nullptr;
  Function *push_back_func = nullptr;
  FunctionRemap *push_back_remap = nullptr;
  const InterrogateFunctionWrapper *push_back_wrapper = nullptr;
  Function *clear_func = nullptr;
  FunctionRemap *clear_remap = nullptr;
  const InterrogateFunctionWrapper *clear_wrapper = nullptr;
  const InterrogateFunctionWrapper *indexer_length_wrapper = nullptr;
  const InterrogateFunctionWrapper *indexer_element_wrapper = nullptr;

  auto get_first_legal_wrapper = [&](Function *func) -> const InterrogateFunctionWrapper * {
    if (func == nullptr) {
      return nullptr;
    }
    InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
    int num_wrappers = func->_ifunc.number_of_c_wrappers();
    for (int wi = 0; wi < num_wrappers; ++wi) {
      FunctionWrapperIndex wrapper_index = func->_ifunc.get_c_wrapper(wi);
      if (wrapper_index == 0) {
        continue;
      }
      const InterrogateFunctionWrapper &wrapper = idb->get_wrapper(wrapper_index);
      if (!is_wrapper_legal_csharp(wrapper)) {
        continue;
      }
      return &wrapper;
    }
    return nullptr;
  };

  bool use_collection_helpers = false;

  std::set<TypeIndex> method_search_seen;
  Function *size_func = find_method_on_type_recursive(this, itype, "size", method_search_seen);
  if (indexer_length_remap == nullptr && size_func != nullptr) {
    indexer_length_func = size_func;
    indexer_length_remap = best_legal_method_remap(size_func);
    if (indexer_length_remap == nullptr) {
      indexer_length_wrapper = get_first_legal_wrapper(size_func);
    }
  }
  method_search_seen.clear();
  Function *get_element_func = find_method_on_type_recursive(this, itype, "get_element", method_search_seen);
  if (indexer_element_remap == nullptr && get_element_func != nullptr) {
    indexer_element_func = get_element_func;
    indexer_element_remap = best_legal_method_remap(get_element_func);
    if (indexer_element_remap == nullptr) {
      indexer_element_wrapper = get_first_legal_wrapper(get_element_func);
    }
  }

  if (is_mutable) {
    method_search_seen.clear();
    set_element_func = find_method_on_type_recursive(this, itype, "set_element", method_search_seen);
    if (set_element_func != nullptr) {
      set_element_remap = best_legal_method_remap(set_element_func);
      if (set_element_remap == nullptr) {
        set_element_wrapper = get_first_legal_wrapper(set_element_func);
      }
    }
    method_search_seen.clear();
    push_back_func = find_method_on_type_recursive(this, itype, "push_back", method_search_seen);
    if (push_back_func != nullptr) {
      push_back_remap = best_legal_method_remap(push_back_func);
      if (push_back_remap == nullptr) {
        push_back_wrapper = get_first_legal_wrapper(push_back_func);
      }
    }
    method_search_seen.clear();
    clear_func = find_method_on_type_recursive(this, itype, "clear", method_search_seen);
    if (clear_func != nullptr) {
      clear_remap = best_legal_method_remap(clear_func);
      if (clear_remap == nullptr) {
        clear_wrapper = get_first_legal_wrapper(clear_func);
      }
    }
  }

  // Use helpers (which call operator[] / size() / push_back() directly) when
  // the class doesn't provide its own user-defined get_element method.  If
  // get_element IS user-defined we route through it instead so any custom
  // logic stays in the binding.
  if (indexer_element_remap == nullptr && indexer_element_wrapper == nullptr) {
    use_collection_helpers = true;
  }

  out << "  public sealed partial class " << class_name << " : Interrogate."
      << (is_mutable ? "NativeList<" : "NativeReadOnlyList<") << element_type << ">, INativeType<"
      << class_name << "> {\n";

  indent(out, 4) << "public static " << class_name
                 << "? __CreateFromNative(IntPtr ptr, NativeOwnership own) {\n";
  indent(out, 6) << "return ptr == IntPtr.Zero ? null : new " << class_name << "(ptr, own);\n";
  indent(out, 4) << "}\n\n";

  // Emit the default constructor from the collection helper ONLY when the
  // type itself doesn't expose any constructors — typedef-based facades
  // (vector_int, vector_string, etc.) fall into this bucket.  A concrete
  // subclass of std::vector or a PointerToBase-derived wrapper has real
  // C++ constructors that write_constructor() will emit below, and
  // emitting our own Collection_*_empty_constructor version on top would
  // collide with the real default ctor.
  if (use_collection_helpers && object->_constructors.empty() &&
      itype.is_default_constructible()) {
    indent(out, 4) << "public " << class_name << "() : base(NativeMethods."
                   << get_collection_helper_name(itype, "empty_constructor") << "(), NativeOwnership.Owned) {}\n\n";
  }

  indent(out, 4) << "static " << class_name << "? INativeType<" << class_name
                 << ">.CreateFromNative(IntPtr ptr, NativeOwnership own) {\n";
  indent(out, 6) << "return __CreateFromNative(ptr, own);\n";
  indent(out, 4) << "}\n\n";

  indent(out, 4) << "internal " << class_name << "(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {\n";
  indent(out, 4) << "}\n\n";
  Functions::const_iterator fi;
  for (fi = object->_constructors.begin(); fi != object->_constructors.end(); ++fi) {
    write_constructor(out, (*fi), object, 4);
  }
  bool is_blittable_element = is_csharp_blittable_type(element_value_type);

  // The convenience constructors below chain `: this()`, so only emit them when
  // a parameterless constructor actually exists.  Two ways it can: the facade
  // emitted its own (above), or the C++ type published a real default ctor
  // (PTA_uchar does).  ReferenceCountedVector<T> has neither -- every one of its
  // constructors takes a TypeHandle -- and chaining to a ctor that isn't there
  // is a C# compile error.
  bool has_parameterless_ctor =
    use_collection_helpers && object->_constructors.empty() &&
    itype.is_default_constructible();

  if (!has_parameterless_ctor) {
    InterrogateDatabase *idb_ctors = InterrogateDatabase::get_ptr();
    for (Function *ctor : object->_constructors) {
      const InterrogateFunction &ifunc = ctor->_ifunc;
      for (int wi = 0; wi < ifunc.number_of_c_wrappers() && !has_parameterless_ctor; ++wi) {
        const InterrogateFunctionWrapper &w =
          idb_ctors->get_wrapper(ifunc.get_c_wrapper(wi));
        if (w.number_of_parameters() == 0) {
          has_parameterless_ctor = true;
        }
      }
      if (has_parameterless_ctor) {
        break;
      }
    }
  }

  if (is_mutable && use_collection_helpers && has_parameterless_ctor) {
    if (is_blittable_element) {
      // Bulk ReadOnlySpan<T> constructor - single memcpy via native resize + pointer
      indent(out, 4) << "public " << class_name << "(ReadOnlySpan<" << element_type << "> source) : this() {\n";
      indent(out, 6) << "NativeMethods." << get_collection_helper_name(itype, "resize") << "(NativeHandle, source.Length);\n";
      indent(out, 6) << "source.CopyTo(AsSpan());\n";
      indent(out, 4) << "}\n\n";
    }
    indent(out, 4) << "public " << class_name << "(IEnumerable<" << element_type << "> source) : this() {\n";
    indent(out, 6) << "if (source is null) throw new ArgumentNullException(nameof(source));\n";
    indent(out, 6) << "foreach (var item in source) {\n";
    indent(out, 8) << "Add(item);\n";
    indent(out, 6) << "}\n";
    indent(out, 4) << "}\n\n";
  }

  if (use_collection_helpers) {
    if (is_blittable_element) {
      // Bulk ToArray via Span - single memcpy
      indent(out, 4) << "public " << element_type << "[] ToArray() {\n";
      indent(out, 6) << "return AsReadOnlySpan().ToArray();\n";
      indent(out, 4) << "}\n\n";

      indent(out, 4) << "public List<" << element_type << "> ToList() {\n";
      indent(out, 6) << "var list = new List<" << element_type << ">(Count);\n";
      indent(out, 6) << "foreach (var item in AsReadOnlySpan()) list.Add(item);\n";
      indent(out, 6) << "return list;\n";
      indent(out, 4) << "}\n\n";
    } else {
      // Element-by-element for non-primitive types
      indent(out, 4) << "public " << element_type << "[] ToArray() {\n";
      indent(out, 6) << "var arr = new " << element_type << "[Count];\n";
      indent(out, 6) << "for (int i = 0; i < arr.Length; i++) arr[i] = this[i];\n";
      indent(out, 6) << "return arr;\n";
      indent(out, 4) << "}\n\n";

      indent(out, 4) << "public List<" << element_type << "> ToList() {\n";
      indent(out, 6) << "var list = new List<" << element_type << ">(Count);\n";
      indent(out, 6) << "for (int i = 0; i < Count; i++) list.Add(this[i]);\n";
      indent(out, 6) << "return list;\n";
      indent(out, 4) << "}\n\n";
    }
  }

  // Bulk data access for primitive types
  if (use_collection_helpers && is_blittable_element) {
    string helper_name = get_collection_helper_name(itype, "");
    indent(out, 4) << "/// <summary>Returns a read-only span over the native memory. Zero-copy.</summary>\n";
    indent(out, 4) << "public unsafe ReadOnlySpan<" << element_type << "> AsReadOnlySpan() {\n";
    indent(out, 6) << "int count = Count;\n";
    indent(out, 6) << "if (count == 0) return ReadOnlySpan<" << element_type << ">.Empty;\n";
    indent(out, 6) << "return new ReadOnlySpan<" << element_type << ">(NativeMethods." << helper_name << "get_data_ptr(NativeHandle).ToPointer(), count);\n";
    indent(out, 4) << "}\n\n";

    if (is_mutable) {
      indent(out, 4) << "/// <summary>Returns a writable span over the native memory. Zero-copy.</summary>\n";
      indent(out, 4) << "public unsafe Span<" << element_type << "> AsSpan() {\n";
      indent(out, 6) << "int count = Count;\n";
      indent(out, 6) << "if (count == 0) return Span<" << element_type << ">.Empty;\n";
      indent(out, 6) << "return new Span<" << element_type << ">(NativeMethods." << helper_name << "get_data_ptr(NativeHandle).ToPointer(), count);\n";
      indent(out, 4) << "}\n\n";

      indent(out, 4) << "/// <summary>Bulk copy from a span into this collection. Resizes to match.</summary>\n";
      indent(out, 4) << "public void CopyFrom(ReadOnlySpan<" << element_type << "> source) {\n";
      indent(out, 6) << "NativeMethods." << helper_name << "resize(NativeHandle, source.Length);\n";
      indent(out, 6) << "source.CopyTo(AsSpan());\n";
      indent(out, 4) << "}\n\n";
    }
  }


  if (use_collection_helpers) {
    indent(out, 4) << "public override int Count => NativeMethods."
                   << get_collection_helper_name(itype, "size") << "(NativeHandle);\n\n";
  } else if (indexer_make_seq != nullptr && indexer_length_remap != nullptr) {
    indent(out, 4) << "public override int Count => (int)NativeMethods."
                   << get_pinvoke_name(indexer_make_seq->_length_getter, indexer_length_remap)
                   << "(NativeHandle);\n\n";
  } else if (indexer_length_remap != nullptr) {
    indent(out, 4) << "public override int Count => (int)NativeMethods."
                   << get_pinvoke_name(indexer_length_func, indexer_length_remap)
                   << "(NativeHandle);\n\n";
  } else if (indexer_length_func != nullptr && indexer_length_wrapper != nullptr) {
    indent(out, 4) << "public override int Count => (int)NativeMethods."
                   << get_pinvoke_name(indexer_length_func->_ifunc, *indexer_length_wrapper)
                   << "(NativeHandle);\n\n";
  }

  if (use_collection_helpers) {
    string native_call = "NativeMethods." + get_collection_helper_name(itype, "get_element") + "(NativeHandle, index)";
    indent(out, 4) << "protected override " << element_type << " GetItem(int index) {\n";
    string element_type_value = get_collection_element_type(itype, false);
    if (element_type_value == "string") {
      // Pinvoke returns IntPtr (see write_dllimport for collection helpers);
      // convert by copying into a managed string without freeing native mem.
      indent(out, 6) << "IntPtr result = " << native_call << ";\n";
      indent(out, 6) << "return result == IntPtr.Zero ? throw new InvalidOperationException(\"Native method returned null.\") : Marshal.PtrToStringUTF8(result)!;\n";
    } else if (is_csharp_primitive_type(element_type_value)) {
      indent(out, 6) << "return " << native_call << ";\n";
    } else {
      // A PT(T) element arrives ref'd by the helper, so the managed wrapper owns a
      // reference to it; a value element arrives freshly heap-copied, and the
      // wrapper owns the allocation outright.
      string element_true_name;
      detect_collection_facade_kind(itype, element_true_name);
      bool handle_element = !collection_element_pointee(element_true_name).empty();

      indent(out, 6) << "IntPtr result = " << native_call << ";\n";
      indent(out, 6) << "return " << element_value_type << ".__CreateFromNative(result, "
                     << (handle_element ? "NativeOwnership.RefCounted" : "NativeOwnership.Owned") << ")"
                     << " ?? throw new InvalidOperationException(\"Native method returned null.\");\n";
    }
    indent(out, 4) << "}\n\n";
  } else if (indexer_element_remap != nullptr || indexer_element_wrapper != nullptr) {
    string native_call = "NativeMethods." +
      (indexer_element_remap != nullptr
        ? (indexer_make_seq != nullptr
        ? get_pinvoke_name(indexer_make_seq->_element_getter, indexer_element_remap)
        : get_pinvoke_name(indexer_element_func, indexer_element_remap))
        : get_pinvoke_name(indexer_element_func->_ifunc, *indexer_element_wrapper)) + "(";
    bool element_has_this = indexer_element_remap != nullptr
      ? indexer_element_remap->_has_this
      : (indexer_element_wrapper->number_of_parameters() != 0 && indexer_element_wrapper->parameter_is_this(0));
    if (element_has_this) {
      native_call += "NativeHandle, ";
    }
    size_t idx_param = element_has_this ? 1 : 0;
    if (indexer_element_remap != nullptr && idx_param < indexer_element_remap->_parameters.size()) {
      string idx_pinvoke = get_pinvoke_type(
        indexer_element_remap->_parameters[idx_param]._remap->get_new_type(), false);
      if (idx_pinvoke != "int") {
        native_call += "(" + idx_pinvoke + ")";
      }
    } else if (indexer_element_wrapper != nullptr && idx_param < (size_t)indexer_element_wrapper->number_of_parameters()) {
      string idx_pinvoke = get_pinvoke_type(indexer_element_wrapper->parameter_get_type((int)idx_param), false);
      if (idx_pinvoke != "int") {
        native_call += "(" + idx_pinvoke + ")";
      }
    }
    native_call += "index)";

    indent(out, 4) << "protected override " << element_type << " GetItem(int index) {\n";
    CPPType *return_type_cpp = indexer_element_remap != nullptr ? indexer_element_remap->_return_type->get_new_type() : nullptr;
    if (return_type_cpp != nullptr &&
        (TypeManager::is_bool(return_type_cpp) ||
         TypeManager::is_simple(return_type_cpp) ||
         TypeManager::is_enum(return_type_cpp))) {
      if (TypeManager::is_enum(return_type_cpp) && is_enum_type(_objects, return_type_cpp)) {
        indent(out, 6) << "return (" << element_type << ")" << native_call << ";\n";
      } else {
        indent(out, 6) << "return " << native_call << ";\n";
      }
    } else if ((return_type_cpp != nullptr &&
                (TypeManager::is_char_pointer(return_type_cpp) ||
                 TypeManager::is_const_char_pointer(return_type_cpp))) ||
               element_type == "string") {
      indent(out, 6) << "IntPtr result = " << native_call << ";\n";
      indent(out, 6) << "return result == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(result);\n";
    } else if (is_csharp_native_object_type(_objects, return_type_cpp, element_type)) {
      indent(out, 6) << "IntPtr result = " << native_call << ";\n";
      indent(out, 6) << "return " << element_value_type << ".__CreateFromNative(result, "
                     << get_native_ownership_name(indexer_element_remap, false) << ")";
      out << " ?? throw new InvalidOperationException(\"Native method returned null.\");\n";
    } else {
      indent(out, 6) << "return " << native_call << ";\n";
    }
    indent(out, 4) << "}\n\n";
  }

  if (is_mutable) {
    indent(out, 4) << "public override bool IsReadOnly => false;\n\n";

    if (use_collection_helpers) {
      indent(out, 4) << "protected override void SetItem(int index, " << element_type << " value) {\n";
      indent(out, 6) << "NativeMethods." << get_collection_helper_name(itype, "set_element") << "(NativeHandle, index, ";
      string element_type_value = get_collection_element_type(itype, false);
      if (is_csharp_primitive_type(element_type_value) || element_type_value == "string") {
        out << "value";
      } else {
        out << "NativeObject.Unwrap(value)";
      }
      out << ");\n";
      indent(out, 4) << "}\n\n";
    } else if (set_element_remap != nullptr || set_element_wrapper != nullptr) {
      indent(out, 4) << "protected override void SetItem(int index, " << element_type << " value) {\n";
      indent(out, 6) << "NativeMethods."
                     << (set_element_remap != nullptr
                         ? get_pinvoke_name(set_element_func, set_element_remap)
                         : get_pinvoke_name(set_element_func->_ifunc, *set_element_wrapper))
                     << "(NativeHandle, (uint)index, ";
      TypeIndex value_type = set_element_remap != nullptr
        ? get_parameter_type_for_remap(set_element_remap, set_element_remap->_has_this ? 2 : 1)
        : set_element_wrapper->parameter_get_type(set_element_wrapper->parameter_is_this(0) ? 2 : 1);
      if (is_csharp_native_object_type(value_type, element_type)) {
        out << "NativeObject.Unwrap(value)";
      } else {
        out << "value";
      }
      out << ");\n";
      indent(out, 4) << "}\n\n";
    } else {
      indent(out, 4) << "protected override void SetItem(int index, " << element_type << " value) => throw new NotSupportedException();\n\n";
    }

    if (use_collection_helpers) {
      indent(out, 4) << "public override void Add(" << element_type << " item) {\n";
      indent(out, 6) << "NativeMethods." << get_collection_helper_name(itype, "push_back") << "(NativeHandle, ";
      string element_type_value = get_collection_element_type(itype, false);
      if (is_csharp_primitive_type(element_type_value) || element_type_value == "string") {
        out << "item";
      } else {
        out << "NativeObject.Unwrap(item)";
      }
      out << ");\n";
      indent(out, 4) << "}\n\n";
    } else if (push_back_remap != nullptr || push_back_wrapper != nullptr) {
      indent(out, 4) << "public override void Add(" << element_type << " item) {\n";
      indent(out, 6) << "NativeMethods."
                     << (push_back_remap != nullptr
                         ? get_pinvoke_name(push_back_func, push_back_remap)
                         : get_pinvoke_name(push_back_func->_ifunc, *push_back_wrapper))
                     << "(NativeHandle, ";
      TypeIndex value_type = push_back_remap != nullptr
        ? get_parameter_type_for_remap(push_back_remap, push_back_remap->_has_this ? 1 : 0)
        : push_back_wrapper->parameter_get_type(push_back_wrapper->parameter_is_this(0) ? 1 : 0);
      if (is_csharp_native_object_type(value_type, element_type)) {
        out << "NativeObject.Unwrap(item)";
      } else {
        out << "item";
      }
      out << ");\n";
      indent(out, 4) << "}\n\n";
    } else {
      indent(out, 4) << "public override void Add(" << element_type << " item) => throw new NotSupportedException();\n\n";
    }

    if (use_collection_helpers) {
      indent(out, 4) << "public override void Clear() {\n";
      indent(out, 6) << "NativeMethods." << get_collection_helper_name(itype, "clear") << "(NativeHandle);\n";
      indent(out, 4) << "}\n\n";
    } else if (clear_remap != nullptr || clear_wrapper != nullptr) {
      indent(out, 4) << "public override void Clear() {\n";
      indent(out, 6) << "NativeMethods."
                     << (clear_remap != nullptr
                         ? get_pinvoke_name(clear_func, clear_remap)
                         : get_pinvoke_name(clear_func->_ifunc, *clear_wrapper))
                     << "(NativeHandle);\n";
      indent(out, 4) << "}\n\n";
    } else {
      indent(out, 4) << "public override void Clear() => throw new NotSupportedException();\n\n";
    }

    indent(out, 4) << "public override void Insert(int index, " << element_type << " item) => throw new NotSupportedException();\n\n";
    indent(out, 4) << "public override void RemoveAt(int index) => throw new NotSupportedException();\n\n";
  }

  TypeIndex coll_type_index = get_type_index_for_interrogate_type(itype);
  bool coll_is_refcounted = (coll_type_index != 0 && is_type_refcounted(coll_type_index));
  string coll_unref_name = coll_is_refcounted ? get_supplemental_unref_destructor_name(itype) : "";

  indent(out, 4) << "protected override void ReleaseNative() {\n";
  if (!destructor_name.empty()) {
    if (coll_is_refcounted) {
      indent(out, 6) << "if (Ownership == NativeOwnership.RefCounted) {\n";
      indent(out, 8) << "NativeMethods." << coll_unref_name << "(NativeHandle);\n";
      indent(out, 6) << "} else {\n";
      indent(out, 8) << "NativeMethods." << destructor_name << "(NativeHandle);\n";
      indent(out, 6) << "}\n";
    } else {
      indent(out, 6) << "NativeMethods." << destructor_name << "(NativeHandle);\n";
    }
  }
  indent(out, 4) << "}\n";
  out << "  }\n";
}

// Is this type an integer once typedefs like size_t are peeled off?  Used to
// tell a sequence's operator[](int) / size() from a mapping's operator[](key).
static bool is_integral_type_index(TypeIndex ti) {
  ti = unwrap_type_aliases(ti);
  if (ti == 0) {
    return false;
  }
  const InterrogateType &it = InterrogateDatabase::get_ptr()->get_type(ti);
  if (!it.is_atomic()) {
    return false;
  }
  switch (it.get_atomic_token()) {
  case AT_int:
  case AT_char:
  case AT_longlong:
    return true;
  default:
    return false;
  }
}

/**
 * Detects a sequence that lets a class surface as IReadOnlyList<T>, from the
 * serialized wrappers (a binary .in load carries no FunctionRemaps).  Two
 * shapes, in priority order:
 *   1. A MAKE_SEQ: a length getter (arity 0) plus an integer-indexed element
 *      getter (arity 1).
 *   2. The bare sequence protocol: a nullary integral size() paired with an
 *      integer-indexed operator[].  This mirrors interfaceMaker.cxx
 *      check_protocols(), which the Python bindings use to set PT_sequence, so
 *      collections like InputDeviceSet that never declared a MAKE_SEQ still
 *      surface as sequences here, matching Python.
 */
bool InterfaceMakerCSharp::
find_sequence_indexer(Object *object,
    Function *&length_func, const InterrogateFunctionWrapper *&length_w,
    Function *&element_func, const InterrogateFunctionWrapper *&element_w,
    TypeIndex &element_index, string &element_type) {
  length_func = element_func = nullptr;
  length_w = element_w = nullptr;
  element_index = 0;
  element_type.clear();
  if (object == nullptr) {
    return false;
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  // Finds the instance wrapper on func_index with exactly want_arity parameters
  // after `this`.
  auto find_seq_wrapper = [&](FunctionIndex func_index, int want_arity,
                              Function *&out_func) -> const InterrogateFunctionWrapper * {
    out_func = nullptr;
    if (func_index == 0) return nullptr;
    FunctionsByIndex::const_iterator it = _functions.find(func_index);
    if (it == _functions.end()) return nullptr;
    Function *f = it->second;
    int n = f->_ifunc.number_of_c_wrappers();
    for (int wi = 0; wi < n; ++wi) {
      FunctionWrapperIndex widx = f->_ifunc.get_c_wrapper(wi);
      if (widx == 0) continue;
      const InterrogateFunctionWrapper &w = idb->get_wrapper(widx);
      if (w.is_explicit_self() || !is_wrapper_legal_csharp(w)) continue;
      int np = w.number_of_parameters();
      bool has_this = np != 0 && w.parameter_is_this(0);
      if (np - (has_this ? 1 : 0) != want_arity) continue;
      out_func = f;
      return &w;
    }
    return nullptr;
  };

  for (MakeSeq *make_seq : object->_make_seqs) {
    if (make_seq == nullptr) continue;
    Function *lf = nullptr, *ef = nullptr;
    const InterrogateFunctionWrapper *lw =
      find_seq_wrapper(make_seq->_imake_seq.get_length_getter(), 0, lf);
    const InterrogateFunctionWrapper *ew =
      find_seq_wrapper(make_seq->_imake_seq.get_element_getter(), 1, ef);
    if (lw == nullptr || ew == nullptr || !ew->has_return_value()) continue;
    // A collection indexer reads through `this`; both accessors are instance methods.
    if (!lw->parameter_is_this(0) || !ew->parameter_is_this(0)) continue;
    TypeIndex eidx = ew->get_return_type();
    string etype = get_csharp_signature_type(eidx, false);
    if (etype.empty()) continue;
    length_func = lf;
    element_func = ef;
    length_w = lw;
    element_w = ew;
    element_index = eidx;
    element_type = etype;
    return true;
  }

  // Fallback: a nullary integral size() plus an integer-indexed operator[].
  Function *op_func = nullptr, *size_func = nullptr;
  const InterrogateFunctionWrapper *op_w = nullptr, *size_w = nullptr;
  for (Function *m : object->_methods) {
    if (m == nullptr || !m->_ifunc.has_name()) continue;
    const string &mname = m->_ifunc.get_name();
    bool want_op = (op_w == nullptr && mname == "operator []");
    bool want_size = (size_w == nullptr && mname == "size");
    if (!want_op && !want_size) continue;

    int nw = m->_ifunc.number_of_c_wrappers();
    for (int wi = 0; wi < nw; ++wi) {
      FunctionWrapperIndex widx = m->_ifunc.get_c_wrapper(wi);
      if (widx == 0) continue;
      const InterrogateFunctionWrapper &w = idb->get_wrapper(widx);
      if (w.is_explicit_self() || !is_wrapper_legal_csharp(w) ||
          !w.has_return_value() || w.number_of_parameters() == 0 ||
          !w.parameter_is_this(0)) {
        continue;
      }
      // operator[]: `this` + one integral index.
      if (want_op && w.number_of_parameters() == 2 &&
          is_integral_type_index(w.parameter_get_type(1))) {
        op_func = m; op_w = &w; break;
      }
      // size(): `this` only, integral return.
      if (want_size && w.number_of_parameters() == 1 &&
          is_integral_type_index(w.get_return_type())) {
        size_func = m; size_w = &w; break;
      }
    }
  }
  if (op_func != nullptr && size_func != nullptr) {
    TypeIndex eidx = op_w->get_return_type();
    string etype = get_csharp_signature_type(eidx, false);
    if (!etype.empty()) {
      length_func = size_func;
      element_func = op_func;
      length_w = size_w;
      element_w = op_w;
      element_index = eidx;
      element_type = etype;
      return true;
    }
  }
  return false;
}

/**
 *
 */
void InterfaceMakerCSharp::
write_proxy_class(ostream &out, const string &, Object *object) {
  const InterrogateType &itype = object->_itype;
  // class_name is the C# type-identity name used for the class declaration and
  // all self-references (constructor, __CreateFromNative, INativeType<>): for a
  // nested type this is the simple name (AxisState), which resolves correctly
  // inside the reopened outer class.  flat_name keeps the flattened form for
  // the internal opaque helper class, whose name must stay globally unique.
  string class_name = get_simple_class_name(itype);
  string flat_name = get_class_name(itype);
  string base_class = get_base_class_clause(itype);
  string interface_list = get_interface_list(itype);
  string destructor_name = get_destructor_wrapper_name(object);
  string opaque_class_name = "__Opaque_" + flat_name;
  bool is_collection_facade = is_collection_facade_type(itype);

  // A MAKE_SEQ lets the class surface as IReadOnlyList<T>.
  Function *indexer_length_func = nullptr;
  Function *indexer_element_func = nullptr;
  const InterrogateFunctionWrapper *indexer_length_w = nullptr;
  const InterrogateFunctionWrapper *indexer_element_w = nullptr;
  TypeIndex indexer_element_index = 0;
  string indexer_type;
  find_sequence_indexer(object, indexer_length_func, indexer_length_w,
                        indexer_element_func, indexer_element_w,
                        indexer_element_index, indexer_type);

  bool is_abstract = is_abstract_type(itype);
  std::vector<const InterrogateType *> secondary_base_types;
  get_secondary_base_types(itype, secondary_base_types);

  // Use get_secondary_base_types (which handles cross-module database loading) to
  // get the full list of types we need interface implementations for.
  // Then categorize: types that are direct secondary derivations (have upcast functions)
  // vs types on a secondary base's primary chain (share its pointer).
  InterrogateDatabase *idb_collect = InterrogateDatabase::get_ptr();

  // all_secondary_bases: types with their own upcast function (actual secondary derivations)
  // secondary_base_owner: maps each such type to its direct owner (the class where di>0 points to it)
  // primary_chain_aliases: types on a secondary base's primary chain (share its pointer)
  std::set<const InterrogateType *> all_secondary_bases;
  std::map<const InterrogateType *, const InterrogateType *> secondary_base_owner;
  std::map<const InterrogateType *, const InterrogateType *> primary_chain_aliases;

  // get_secondary_base_types already does all the heavy lifting of cross-module resolution.
  // It returns ALL types we need (both actual secondaries and primary-chain aliases).
  // We need to determine which category each type falls into.
  std::vector<const InterrogateType *> full_secondary_list;
  get_secondary_base_types(itype, full_secondary_list);

  // For each type in the list, check if any of its "parent classes" (types whose secondary
  // derivation leads to it) have an upcast function for it. If so, it's a real secondary base.
  // Otherwise, it's a primary-chain alias.
  //
  // The key insight: get_secondary_base_types visits types in order from the visit() call.
  // Secondary bases have upcast functions; primary-chain types don't.
  // We identify actual secondary bases by checking if get_upcast_pinvoke_name succeeds.
  // For that we need to find the owner that has the type as derivation[di>0].

  // Build a lookup by interface name for dedup
  std::set<string> added_names;

  for (const InterrogateType *base_type : full_secondary_list) {
    if (base_type == nullptr) continue;
    string base_name = get_interface_name(*base_type);
    if (!added_names.insert(base_name).second) continue;

    // Try to find an upcast: search all types in the hierarchy for one that has
    // this base_type as a secondary derivation (di > 0)
    bool found_upcast = false;

    // Check itype itself first
    for (int di = 1; di < itype.number_of_derivations(); ++di) {
      TypeIndex deriv_idx = itype.get_derivation(di);
      if (!is_wrapped_type(deriv_idx)) continue;
      const InterrogateType &dt = idb_collect->get_type(deriv_idx);
      if (get_interface_name(dt) == base_name) {
        string upcast = get_upcast_pinvoke_name(itype, di);
        if (!upcast.empty()) {
          all_secondary_bases.insert(base_type);
          secondary_base_owner[base_type] = &itype;
          found_upcast = true;
        }
        break;
      }
    }

    if (!found_upcast) {
      // Search through all types already in all_secondary_bases as potential owners
      for (const InterrogateType *potential_owner : full_secondary_list) {
        if (potential_owner == nullptr || potential_owner == base_type) continue;
        ensure_database_loaded(*potential_owner);
        for (int di = 1; di < potential_owner->number_of_derivations(); ++di) {
          TypeIndex deriv_idx = potential_owner->get_derivation(di);
          if (!is_wrapped_type(deriv_idx)) continue;
          const InterrogateType &dt = idb_collect->get_type(deriv_idx);
          if (get_interface_name(dt) == base_name) {
            string upcast = get_upcast_pinvoke_name(*potential_owner, di);
            if (!upcast.empty()) {
              all_secondary_bases.insert(base_type);
              secondary_base_owner[base_type] = potential_owner;
              found_upcast = true;
            }
            break;
          }
        }
        if (found_upcast) break;
      }
    }

    if (!found_upcast) {
      // Also check the primary-base chain of itype for potential owners
      const InterrogateType *cur = &itype;
      while (!found_upcast) {
        ensure_database_loaded(*cur);
        if (cur->number_of_derivations() == 0) break;
        TypeIndex primary_idx = cur->get_derivation(0);
        if (!is_wrapped_type(primary_idx)) break;
        cur = &idb_collect->get_type(primary_idx);
        for (int di = 1; di < cur->number_of_derivations(); ++di) {
          TypeIndex deriv_idx = cur->get_derivation(di);
          if (!is_wrapped_type(deriv_idx)) continue;
          const InterrogateType &dt = idb_collect->get_type(deriv_idx);
          if (get_interface_name(dt) == base_name) {
            string upcast = get_upcast_pinvoke_name(*cur, di);
            if (!upcast.empty()) {
              all_secondary_bases.insert(base_type);
              secondary_base_owner[base_type] = cur;
              found_upcast = true;
            }
            break;
          }
        }
      }
    }

    if (!found_upcast) {
      // This type is on a primary chain of some secondary base — find which one
      for (const InterrogateType *sec : all_secondary_bases) {
        if (get_interface_name(*sec) == base_name) { found_upcast = true; break; }
      }
      if (!found_upcast) {
        // Find the nearest secondary base whose primary chain includes this type
        for (const InterrogateType *sec : all_secondary_bases) {
          primary_chain_aliases[base_type] = sec;
          break;
        }
      }
    }
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();

  if (itype.has_comment()) {
    emit_xml_doc_comment(out, itype.get_comment(), 2);
  }

  out << "  " << (is_collection_facade ? "internal " : "public ");
  if (is_abstract) {
    out << "abstract ";
  }
  out << "partial class " << class_name << " : " << base_class;
  if (uses_csharp_interface(itype)) {
    out << ", " << get_simple_interface_name(itype);
  }
  out << ", INativeType<" << class_name << ">" << interface_list;
  // The type that declares is_of_type (a runtime type system, e.g. dtool's TypedObject) implements
  // IRuntimeTyped so that CastTo<T> can verify the dynamic type before downcasting. Derived types
  // inherit the interface (and the is_of_type implementation), so it's only declared at the root.
  if (type_declares_method(itype, "is_of_type")) {
    out << ", Interrogate.IRuntimeTyped";
  }
  if (is_collection_facade) {
    string collection_interface = get_collection_interface_type(itype, false);
    if (collection_interface.size() >= 2 && collection_interface.substr(collection_interface.size() - 2) == "?") {
      collection_interface = collection_interface.substr(0, collection_interface.size() - 1);
    }
    out << ", " << collection_interface;
  } else if (!indexer_type.empty()) {
    out << ", IReadOnlyList<" << indexer_type << ">";
  }
  if (base_class == "NativeObject") {
    out << ", IDisposable";
  }
  out << " {\n";

  // Helper to get field name for a type
  auto get_field_name = [&](const InterrogateType *t) -> string {
    string iface = get_interface_name(*t);
    string field = "_" + iface.substr(1);
    field[1] = std::tolower((unsigned char)field[1]);
    field += "Ptr";
    return field;
  };

  // Generate private fields for actual secondary bases
  for (const InterrogateType *sec_base : all_secondary_bases) {
    if (sec_base == nullptr) continue;
    indent(out, 4) << "private IntPtr " << get_field_name(sec_base) << ";\n";
  }

  // Generate explicit interface implementations
  for (const InterrogateType *sec_base : all_secondary_bases) {
    if (sec_base == nullptr) continue;
    indent(out, 4) << "IntPtr " << get_interface_name(*sec_base) << ".NativeHandle => " << get_field_name(sec_base) << ";\n";
  }
  // Aliases share the pointer of their nearest secondary-base ancestor
  for (auto it = primary_chain_aliases.begin(); it != primary_chain_aliases.end(); ++it) {
    const InterrogateType *alias_type = it->first;
    const InterrogateType *sec_base = it->second;
    indent(out, 4) << "IntPtr " << get_interface_name(*alias_type) << ".NativeHandle => " << get_field_name(sec_base) << ";\n";
  }

  if (!all_secondary_bases.empty() || !primary_chain_aliases.empty()) {
    out << "\n";
  }

  if (is_abstract) {
    indent(out, 4) << "internal sealed class " << opaque_class_name << " : " << class_name << " {\n";
    indent(out, 6) << "internal " << opaque_class_name << "(IntPtr ptr, NativeOwnership own) : base(ptr, own) {\n";
    indent(out, 6) << "}\n";
    TypeIndex proxy_type_index = get_type_index_for_interrogate_type(itype);
    bool proxy_is_refcounted = (proxy_type_index != 0 && is_type_refcounted(proxy_type_index));
    string proxy_unref_name = proxy_is_refcounted ? get_supplemental_unref_destructor_name(itype) : "";

    indent(out, 6) << "protected override void ReleaseNative() {\n";
    if (!destructor_name.empty()) {
      if (proxy_is_refcounted) {
        indent(out, 8) << "if (Ownership == NativeOwnership.RefCounted) {\n";
        indent(out, 10) << "NativeMethods." << proxy_unref_name << "(NativeHandle);\n";
        indent(out, 8) << "} else {\n";
        indent(out, 10) << "NativeMethods." << destructor_name << "(NativeHandle);\n";
        indent(out, 8) << "}\n";
      } else {
        indent(out, 8) << "NativeMethods." << destructor_name << "(NativeHandle);\n";
      }
    } else if (base_class != "NativeObject") {
      indent(out, 8) << "base.ReleaseNative();\n";
    }
    indent(out, 6) << "}\n";
    indent(out, 4) << "}\n\n";
  }

  indent(out, 4) << "public static ";
  if (base_class != "NativeObject") {
    out << "new ";
  }
  out << class_name << "? __CreateFromNative(IntPtr ptr, NativeOwnership own) {\n";
  indent(out, 6) << "return ptr == IntPtr.Zero ? null : ";
  if (is_abstract) {
    out << "new " << opaque_class_name << "(ptr, own);\n";
  } else {
    out << "new " << class_name << "(ptr, own);\n";
  }
  indent(out, 4) << "}\n\n";

  indent(out, 4) << "static " << class_name << "? INativeType<" << class_name
                 << ">.CreateFromNative(IntPtr ptr, NativeOwnership own) {\n";
  indent(out, 6) << "return __CreateFromNative(ptr, own);\n";
  indent(out, 4) << "}\n\n";

  // Expose the registered runtime type handle so CastTo<T> can do a checked downcast. Only for types
  // in a runtime type system (those exposing get_class_type/is_of_type); others keep TypeHandle == 0.
  if (type_has_method(itype, "get_class_type") &&
      type_has_method(itype, "is_of_type")) {
    indent(out, 4) << "static int INativeType<" << class_name
                   << ">.TypeHandle => GetClassType();\n\n";
  }

  indent(out, 4) << "internal " << class_name
                 << "(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {\n";

  // Initialize ALL secondary base pointers (direct and inherited).
  // Each upcast function expects a pointer to the owner type. If the owner is reachable
  // from itype via only primary bases, that's `ptr` (same address). If the owner is itself
  // a secondary base, we must use the already-computed pointer for that base.

  // We will use a bunch of inline helper functions to make things easier here.
  // Helper: check if 'target' is on the primary base chain of 'from' (by interface name)
  auto is_on_primary_chain = [&](const InterrogateType &from, const InterrogateType *target) -> bool {
    string target_name = get_interface_name(*target);
    const InterrogateType *cur = &from;
    while (cur != nullptr) {
      if (get_interface_name(*cur) == target_name) return true;
      ensure_database_loaded(*cur);
      if (cur->number_of_derivations() == 0) break;
      TypeIndex primary_idx = cur->get_derivation(0);
      if (!is_wrapped_type(primary_idx)) break;
      cur = &idb->get_type(primary_idx);
    }
    return false;
  };

  // Helper: get the field name for a type
  auto get_field_name_for = [&](const InterrogateType *t) -> string {
    string iface = get_interface_name(*t);
    string field = "_" + iface.substr(1);
    field[1] = std::tolower((unsigned char)field[1]);
    field += "Ptr";
    return field;
  };

  // Helper: find the pointer expression to use as input to an upcast owned by 'owner'.
  // Returns "ptr" if owner is on itype's primary chain, or a field name if owner is
  // reachable through a secondary base.
  auto get_owner_ptr_expr = [&](const InterrogateType *owner) -> string {
    if (is_on_primary_chain(itype, owner)) {
      return "ptr";
    }
    for (const InterrogateType *sec : all_secondary_bases) {
      if (sec == nullptr) continue;
      if (is_on_primary_chain(*sec, owner)) {
        return get_field_name_for(sec);
      }
    }
    return "ptr";
  };

  // Process in order: direct secondary bases first (they use ptr or primary chain),
  // then inherited ones (which may depend on already-computed fields).
  // The set iteration order may not guarantee this, so process in two passes:
  // Pass 1: bases whose owner is on itype's primary chain (always use ptr)
  // Pass 2: bases whose owner is a secondary base (use that base's field)
  std::vector<const InterrogateType *> pass1, pass2;
  for (const InterrogateType *sec_base : all_secondary_bases) {
    if (sec_base == nullptr) continue;
    auto owner_it = secondary_base_owner.find(sec_base);
    if (owner_it == secondary_base_owner.end()) continue;
    if (is_on_primary_chain(itype, owner_it->second)) {
      pass1.push_back(sec_base);
    } else {
      pass2.push_back(sec_base);
    }
  }

  auto emit_upcast_init = [&](const InterrogateType *sec_base) {
    auto owner_it = secondary_base_owner.find(sec_base);
    if (owner_it == secondary_base_owner.end()) return;
    const InterrogateType *owner = owner_it->second;

    // Find the derivation index in the owner class (match by interface name, not pointer)
    string target_name = get_interface_name(*sec_base);
    int owner_derivation_index = -1;
    ensure_database_loaded(*owner);
    for (int di = 1; di < owner->number_of_derivations(); ++di) {
      TypeIndex deriv_idx = owner->get_derivation(di);
      if (!is_wrapped_type(deriv_idx)) continue;
      const InterrogateType &deriv_type = idb->get_type(deriv_idx);
      if (get_interface_name(deriv_type) == target_name) {
        owner_derivation_index = di;
        break;
      }
    }
    if (owner_derivation_index < 0) return;

    string upcast_func = get_upcast_pinvoke_name(*owner, owner_derivation_index);
    if (upcast_func.empty()) return;

    string field_name = get_field_name_for(sec_base);
    string input_ptr = get_owner_ptr_expr(owner);

    indent(out, 6) << field_name << " = " << upcast_func << "(" << input_ptr << ");\n";
  };

  for (const InterrogateType *sec_base : pass1) emit_upcast_init(sec_base);
  for (const InterrogateType *sec_base : pass2) emit_upcast_init(sec_base);

  indent(out, 4) << "}\n\n";

  Functions::const_iterator fi;
  if (!is_abstract) {
    for (fi = object->_constructors.begin(); fi != object->_constructors.end(); ++fi) {
      write_constructor(out, (*fi), object, 4);
    }
  }

  if (!is_abstract && !object->_constructors.empty()) {
    out << "\n";
  }

  {
    std::set<string> method_signatures;
    InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
    auto reserve_property_names = [&](const InterrogateType &source_type) {
      for (int i = 0; i < source_type.number_of_elements(); ++i) {
        const InterrogateElement &ielement = idb->get_element(source_type.get_element(i));
        method_signatures.insert("N:" +
          to_pascal_case(make_csharp_identifier(ielement.get_name())));
      }
    };
    auto reserve_method_signatures = [&](Function *method) {
      if (method == nullptr) {
        return;
      }

      string reserved_name = make_csharp_identifier(method->_ifunc.get_name());
      Function::Remaps::const_iterator ri;
      for (ri = method->_remaps.begin(); ri != method->_remaps.end(); ++ri) {
        FunctionRemap *remap = (*ri);
        if ((remap->_flags & FunctionRemap::F_explicit_self) != 0 ||
            remap->_type == FunctionRemap::T_constructor ||
            remap->_type == FunctionRemap::T_destructor ||
            !is_remap_legal_csharp(remap)) {
          continue;
        }

        bool is_static = !remap->_has_this;
        size_t first_param = remap->_has_this ? 1 : 0;
        std::vector<string> reserved_param_types;
        for (size_t i = first_param; i < remap->_parameters.size(); ++i) {
          reserved_param_types.push_back(
            get_csharp_signature_type_for_wrapper(remap->_parameters[i]._remap, false));
        }

        method_signatures.insert("R:" +
          build_signature_key(reserved_name, reserved_param_types, is_static));
      }
    };

    reserve_property_names(itype);
    for (fi = object->_methods.begin(); fi != object->_methods.end(); ++fi) {
      reserve_method_signatures(*fi);
    }
    for (const InterrogateType *base_type : secondary_base_types) {
      if (base_type == nullptr) {
        continue;
      }

      reserve_property_names(*base_type);
      for (int mi = 0; mi < base_type->number_of_methods(); ++mi) {
        reserve_method_signatures(record_function(*base_type, base_type->get_method(mi)));
      }
      for (int ci = 0; ci < base_type->number_of_casts(); ++ci) {
        reserve_method_signatures(record_function(*base_type, base_type->get_cast(ci)));
      }
    }

    for (fi = object->_methods.begin(); fi != object->_methods.end(); ++fi) {
      // operator[] is redundant with the public indexer emitted for the sequence.
      if (!indexer_type.empty() && !is_collection_facade && (*fi)->_ifunc.has_name() &&
          make_csharp_identifier((*fi)->_ifunc.get_name()) == "op_index") {
        continue;
      }
      write_method(out, (*fi), object, 4, false, &method_signatures);
    }
    for (const InterrogateType *base_type : secondary_base_types) {
      if (base_type == nullptr) {
        continue;
      }

      for (int mi = 0; mi < base_type->number_of_methods(); ++mi) {
        Function *base_method = record_function(*base_type, base_type->get_method(mi));
        write_method(out, base_method, object, 4, false, &method_signatures);
      }
      for (int ci = 0; ci < base_type->number_of_casts(); ++ci) {
        Function *base_cast = record_function(*base_type, base_type->get_cast(ci));
        write_method(out, base_cast, object, 4, false, &method_signatures);
      }
    }

    write_operator_aliases(out, object, 4, &method_signatures, false);
  }

  std::set<string> emitted_proxy_props;
  auto emit_properties_for_type = [&](const InterrogateType &source_type) {
    int num_elements = source_type.number_of_elements();
    for (int i = 0; i < num_elements; ++i) {
      const InterrogateElement &ielement = idb->get_element(source_type.get_element(i));
      Property property(ielement);

      if (ielement.has_getter()) {
        FunctionsByIndex::const_iterator fi2 = _functions.find(ielement.get_getter());
        if (fi2 == _functions.end()) {
          record_function(source_type, ielement.get_getter());
          fi2 = _functions.find(ielement.get_getter());
        }
        if (fi2 != _functions.end()) {
          Function *func = (*fi2).second;
          property._has_this |= func->_has_this;
          Function::Remaps::const_iterator ri;
          for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
            FunctionRemap *remap = (*ri);
            if ((remap->_flags & FunctionRemap::F_explicit_self) == 0) {
              property._getter_remaps.push_back(remap);
            }
          }
        }
      }

      if (ielement.has_setter()) {
        FunctionsByIndex::const_iterator fi2 = _functions.find(ielement.get_setter());
        if (fi2 == _functions.end()) {
          record_function(source_type, ielement.get_setter());
          fi2 = _functions.find(ielement.get_setter());
        }
        if (fi2 != _functions.end()) {
          Function *func = (*fi2).second;
          property._has_this |= func->_has_this;
          Function::Remaps::const_iterator ri;
          for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
            FunctionRemap *remap = (*ri);
            if ((remap->_flags & FunctionRemap::F_explicit_self) == 0) {
              property._setter_remaps.push_back(remap);
            }
          }
        }
      }

      string pn = to_pascal_case(make_csharp_identifier(ielement.get_name()));
      if (emitted_proxy_props.insert(pn).second) {
        if (property._getter_remaps.empty() && property._setter_remaps.empty()) {
          // See the interface loop: fall back to .in C-wrappers for synthesized data-member accessors.
          write_property_from_wrapper(out, ielement, object, 4, false);
        } else {
          write_property(out, &property, object, 4, false);
        }
      }
    }
  };

  emit_properties_for_type(itype);
  for (const InterrogateType *base_type : secondary_base_types) {
    if (base_type != nullptr) {
      emit_properties_for_type(*base_type);
    }
  }

  if (!indexer_type.empty() && !is_collection_facade) {
    string element_this = get_native_this_argument(itype, indexer_element_func->_ifunc);
    string length_this = get_native_this_argument(itype, indexer_length_func->_ifunc);
    string index_cast = get_pinvoke_type(indexer_element_w->parameter_get_type(1), false);
    string index_arg = (index_cast == "int") ? "index" : "(" + index_cast + ")index";
    string element_call =
      get_pinvoke_call_name(indexer_element_func->_ifunc, *indexer_element_w) +
      "(" + element_this + ", " + index_arg + ")";

    string concrete_type = get_csharp_native_object_class_name(indexer_element_index);
    string element_expr;
    if (!concrete_type.empty()) {
      element_expr = concrete_type + ".__CreateFromNative(" + element_call + ", " +
                     get_native_ownership_name(*indexer_element_w, false) + ")!";
    } else if (is_csharp_enum_type(indexer_element_index)) {
      element_expr = "(" + indexer_type + ")" + element_call;
    } else if ((indexer_type == "string" || indexer_type == "string?") &&
               get_pinvoke_type(indexer_element_index, true) != "string") {
      element_expr = "Marshal.PtrToStringUTF8(" + element_call + ") ?? string.Empty";
    } else {
      element_expr = element_call;
    }

    // Public indexer: implicitly implements IReadOnlyList<T>.this[int] and
    // supersedes the C++ operator[] (whose op_index method is suppressed below).
    indent(out, 4) << "public " << indexer_type << " this[int index] {\n";
    indent(out, 6) << "get { return " << element_expr << "; }\n";
    indent(out, 4) << "}\n\n";

    // Count implements IReadOnlyCollection<T>.Count.  Emit it as a public
    // property so `collection.Count` works directly, unless the wrapped class
    // already surfaces a member whose C# (PascalCase) name is "Count" -- then
    // fall back to an explicit interface implementation to avoid the collision.
    bool count_collides = false;
    {
      InterrogateDatabase *idb2 = InterrogateDatabase::get_ptr();
      for (Function *seq_m : object->_methods) {
        if (seq_m != nullptr && seq_m->_ifunc.has_name() &&
            to_pascal_case(make_csharp_identifier(seq_m->_ifunc.get_name())) == "Count") {
          count_collides = true;
          break;
        }
      }
      for (int pei = 0; !count_collides && pei < itype.number_of_elements(); ++pei) {
        const InterrogateElement &pel = idb2->get_element(itype.get_element(pei));
        if (to_pascal_case(make_csharp_identifier(pel.get_name())) == "Count") {
          count_collides = true;
        }
      }
    }
    if (count_collides) {
      indent(out, 4) << "int IReadOnlyCollection<" << indexer_type << ">.Count {\n";
    } else {
      indent(out, 4) << "public int Count {\n";
    }
    indent(out, 6) << "get { return (int)"
                   << get_pinvoke_call_name(indexer_length_func->_ifunc, *indexer_length_w)
                   << "(" << length_this << "); }\n";
    indent(out, 4) << "}\n\n";

    indent(out, 4) << "IEnumerator<" << indexer_type << "> IEnumerable<"
                   << indexer_type << ">.GetEnumerator() {\n";
    indent(out, 6) << "int count = ((IReadOnlyCollection<" << indexer_type
                   << ">)this).Count;\n";
    indent(out, 6) << "for (int i = 0; i < count; i++) {\n";
    indent(out, 8) << "yield return this[i];\n";
    indent(out, 6) << "}\n";
    indent(out, 4) << "}\n\n";

    indent(out, 4) << "IEnumerator IEnumerable.GetEnumerator() {\n";
    indent(out, 6) << "return ((IEnumerable<" << indexer_type
                   << ">)this).GetEnumerator();\n";
    indent(out, 4) << "}\n\n";
  }

  write_dispose_pattern(out, object, 4);
  out << "  }\n";
}

/**
 *
 */
void InterfaceMakerCSharp::
write_constructor(ostream &out, Function *func, Object *object, int indent_level) {
  // The C# constructor name must match the (possibly nested) simple class name.
  string class_name = get_simple_class_name(object->_itype);
  std::set<string> emitted_signatures;
  std::vector<string> internal_ctor_param_types;
  internal_ctor_param_types.push_back("IntPtr");
  internal_ctor_param_types.push_back("NativeOwnership");
  emitted_signatures.insert("M:" +
    build_signature_key(class_name, internal_ctor_param_types, false));

  Function::Remaps::const_iterator ri;
  for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
    FunctionRemap *remap = (*ri);
    const InterrogateFunctionWrapper *wrapper = get_wrapper_for_remap(remap);
    if (
        (remap->_flags & FunctionRemap::F_explicit_self) != 0 ||
        !is_remap_legal_csharp(remap)) {
      continue;
    }

    std::vector<string> param_types;
    std::vector<string> param_decls;
    std::vector<string> param_names;
    std::vector<string> native_args;

    struct StreamBridgeSite { string var; string factory; string param; };
    std::vector<StreamBridgeSite> stream_bridges;

    for (size_t i = 0; i < remap->_parameters.size(); ++i) {
      ParameterRemap *param_remap = remap->_parameters[i]._remap;
      TypeIndex param_type_index = get_parameter_type_for_remap(remap, i);
      bool param_nullable = is_parameter_nullable(remap, i);
      string param_type = (param_type_index != 0)
        ? get_csharp_signature_type(param_type_index, param_nullable, /*is_parameter=*/true)
        : get_csharp_signature_type_for_wrapper(param_remap, param_nullable);

      AtomicToken stream_tok = csharp_stream_token_for_type(param_type_index);
      if (stream_tok != AT_not_atomic) {
        param_type = "global::System.IO.Stream";
      }

      string param_name = get_csharp_parameter_name(remap, i);
      param_types.push_back(param_type);
      param_decls.push_back(param_type + " " + param_name);
      param_names.push_back(param_name);

      if (stream_tok != AT_not_atomic) {
        string bridge_var = "__p3stream" + std::to_string(stream_bridges.size());
        stream_bridges.push_back({bridge_var, csharp_stream_bridge_factory(stream_tok),
                                  param_name});
        native_args.push_back(bridge_var + ".Handle");
      } else if (is_csharp_native_object_type(param_type_index, param_type) ||
                 is_collection_type_index(param_type_index)) {
        if (!param_type.empty() && param_type[0] == 'I' && param_type.size() > 1 && isupper(param_type[1])) {
          string as_type = param_type;
          if (!as_type.empty() && as_type.back() == '?') {
            as_type.pop_back();
          }
          native_args.push_back("(" + param_name + " as " + as_type + ")?.NativeHandle ?? IntPtr.Zero");
        } else {
          native_args.push_back("NativeObject.Unwrap(" + param_name + ")");
        }
      } else if (!is_csharp_primitive_type(param_type) && is_csharp_enum_type(param_type_index)) {
        native_args.push_back("(int)" + param_name);
      } else {
        native_args.push_back(param_name);
      }
    }

    {
      bool has_skipped_param = false;
      for (const string &pt : param_types) {
        if (pt.empty()) { has_skipped_param = true; break; }
      }
      if (has_skipped_param) {
        continue;
      }
    }

    string signature_key = build_signature_key(class_name, param_types, false);
    if (!emitted_signatures.insert("M:" + signature_key).second) {
      continue;
    }

    if (func->_ifunc.has_comment()) {
      emit_xml_doc_comment(out, func->_ifunc.get_comment(), indent_level);
    }

    string ownership =
      (wrapper != nullptr) ? get_native_ownership_name(*wrapper, true)
                           : get_native_ownership_name(remap, true);

    if (stream_bridges.empty()) {
      // No stream bridges needed — fast path with chained `: this(…)`.
      indent(out, indent_level) << "public " << class_name << "(";
      for (size_t i = 0; i < param_decls.size(); ++i) {
        if (i != 0) { out << ", "; }
        out << param_decls[i];
      }
      out << ") : this(NativeMethods." << get_pinvoke_name(func, remap) << "(";
      for (size_t i = 0; i < native_args.size(); ++i) {
        if (i != 0) { out << ", "; }
        out << native_args[i];
      }
      out << "), " << ownership << ") {\n";
      indent(out, indent_level) << "}\n\n";
    } else {
      // Stream params need `using var` lifetime management, which a chained
      // initializer can't provide.  Route through a private static helper
      // that owns the bridge for the duration of the native call.
      string helper_name = "__p3Create" + get_pinvoke_name(func, remap);
      indent(out, indent_level) << "public " << class_name << "(";
      for (size_t i = 0; i < param_decls.size(); ++i) {
        if (i != 0) { out << ", "; }
        out << param_decls[i];
      }
      out << ") : this(" << helper_name << "(";
      for (size_t i = 0; i < param_names.size(); ++i) {
        if (i != 0) { out << ", "; }
        out << param_names[i];
      }
      out << "), " << ownership << ") {\n";
      indent(out, indent_level) << "}\n";

      indent(out, indent_level) << "private static IntPtr " << helper_name << "(";
      for (size_t i = 0; i < param_decls.size(); ++i) {
        if (i != 0) { out << ", "; }
        out << param_decls[i];
      }
      out << ") {\n";
      for (const auto &b : stream_bridges) {
        indent(out, indent_level + 2) << "using var " << b.var << " = "
                                      << b.factory << "(" << b.param << ");\n";
      }
      indent(out, indent_level + 2) << "return NativeMethods."
                                    << get_pinvoke_name(func, remap) << "(";
      for (size_t i = 0; i < native_args.size(); ++i) {
        if (i != 0) { out << ", "; }
        out << native_args[i];
      }
      out << ");\n";
      indent(out, indent_level) << "}\n\n";
    }
  }

  if (!func->_remaps.empty()) {
    return;
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  int num_wrappers = func->_ifunc.number_of_c_wrappers();
  for (int wi = 0; wi < num_wrappers; ++wi) {
    FunctionWrapperIndex wrapper_index = func->_ifunc.get_c_wrapper(wi);
    if (wrapper_index == 0) {
      continue;
    }

    const InterrogateFunctionWrapper &wrapper = idb->get_wrapper(wrapper_index);
    if (wrapper.is_explicit_self() || !is_wrapper_legal_csharp(wrapper)) {
      continue;
    }

    std::vector<string> param_types;
    std::vector<string> param_decls;
    std::vector<string> param_names;
    std::vector<string> native_args;

    struct StreamBridgeSite { string var; string factory; string param; };
    std::vector<StreamBridgeSite> stream_bridges;

    for (int i = 0; i < wrapper.number_of_parameters(); ++i) {
      TypeIndex param_type_index = wrapper.parameter_get_type(i);
      string param_type = get_csharp_signature_type(param_type_index, wrapper.parameter_is_nullable(i), /*is_parameter=*/true);

      AtomicToken stream_tok = csharp_stream_token_for_type(param_type_index);
      if (stream_tok != AT_not_atomic) {
        param_type = "global::System.IO.Stream";
      }

      string param_name = wrapper.parameter_has_name(i)
        ? make_csharp_identifier(wrapper.parameter_get_name(i))
        : string("param") + std::to_string(i);
      param_types.push_back(param_type);
      param_decls.push_back(param_type + " " + param_name);
      param_names.push_back(param_name);

      if (stream_tok != AT_not_atomic) {
        string bridge_var = "__p3stream" + std::to_string(stream_bridges.size());
        stream_bridges.push_back({bridge_var, csharp_stream_bridge_factory(stream_tok),
                                  param_name});
        native_args.push_back(bridge_var + ".Handle");
      } else if (is_csharp_native_object_type(param_type_index, param_type) ||
                 is_collection_type_index(param_type_index)) {
        if (!param_type.empty() && param_type[0] == 'I' && param_type.size() > 1 && isupper(param_type[1])) {
          string as_type = param_type;
          if (!as_type.empty() && as_type.back() == '?') {
            as_type.pop_back();
          }
          native_args.push_back("(" + param_name + " as " + as_type + ")?.NativeHandle ?? IntPtr.Zero");
        } else {
          native_args.push_back("NativeObject.Unwrap(" + param_name + ")");
        }
      } else if (!is_csharp_primitive_type(param_type) && is_csharp_enum_type(param_type_index)) {
        native_args.push_back("(int)" + param_name);
      } else {
        native_args.push_back(param_name);
      }
    }

    {
      bool has_skipped_param = false;
      for (const string &pt : param_types) {
        if (pt.empty()) { has_skipped_param = true; break; }
      }
      if (has_skipped_param) {
        continue;
      }
    }

    string signature_key = build_signature_key(class_name, param_types, false);
    if (!emitted_signatures.insert("M:" + signature_key).second) {
      continue;
    }

    if (func->_ifunc.has_comment()) {
      emit_xml_doc_comment(out, func->_ifunc.get_comment(), indent_level);
    }

    string ownership = get_native_ownership_name(wrapper, true);

    if (stream_bridges.empty()) {
      indent(out, indent_level) << "public " << class_name << "(";
      for (size_t i = 0; i < param_decls.size(); ++i) {
        if (i != 0) { out << ", "; }
        out << param_decls[i];
      }
      out << ") : this(NativeMethods." << get_pinvoke_name(func->_ifunc, wrapper) << "(";
      for (size_t i = 0; i < native_args.size(); ++i) {
        if (i != 0) { out << ", "; }
        out << native_args[i];
      }
      out << "), " << ownership << ") {\n";
      indent(out, indent_level) << "}\n\n";
    } else {
      string helper_name = "__p3Create" + get_pinvoke_name(func->_ifunc, wrapper);
      indent(out, indent_level) << "public " << class_name << "(";
      for (size_t i = 0; i < param_decls.size(); ++i) {
        if (i != 0) { out << ", "; }
        out << param_decls[i];
      }
      out << ") : this(" << helper_name << "(";
      for (size_t i = 0; i < param_names.size(); ++i) {
        if (i != 0) { out << ", "; }
        out << param_names[i];
      }
      out << "), " << ownership << ") {\n";
      indent(out, indent_level) << "}\n";

      indent(out, indent_level) << "private static IntPtr " << helper_name << "(";
      for (size_t i = 0; i < param_decls.size(); ++i) {
        if (i != 0) { out << ", "; }
        out << param_decls[i];
      }
      out << ") {\n";
      for (const auto &b : stream_bridges) {
        indent(out, indent_level + 2) << "using var " << b.var << " = "
                                      << b.factory << "(" << b.param << ");\n";
      }
      indent(out, indent_level + 2) << "return NativeMethods."
                                    << get_pinvoke_name(func->_ifunc, wrapper) << "(";
      for (size_t i = 0; i < native_args.size(); ++i) {
        if (i != 0) { out << ", "; }
        out << native_args[i];
      }
      out << ");\n";
      indent(out, indent_level) << "}\n\n";
    }
  }
}

void InterfaceMakerCSharp::
write_operator_aliases(ostream &out, Object *object, int indent_level,
                       std::set<string> *emitted_signatures,
                       bool is_interface) {
  if (object == nullptr) {
    return;
  }

  string class_name = get_simple_class_name(object->_itype);
  string declaring_type_name = is_interface
    ? get_simple_interface_name(object->_itype)
    : class_name;
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  std::vector<CSharpOperatorCandidate> candidates;
  std::set<string> candidate_keys;
  auto is_native_object_type = [&](TypeIndex type_index) {
    type_index = unwrap_type_aliases(type_index);
    if (type_index == 0) {
      return false;
    }
    const InterrogateType &itype = idb->get_type(type_index);
    return itype.is_class() || itype.is_struct() || itype.is_union();
  };
  auto add_candidate = [&](CSharpOperatorCandidate &&candidate) {
    size_t arity = candidate.parameters.size();
    bool valid = false;
    if (is_equality_csharp_operator(candidate.symbol)) {
      valid = arity == 2 && candidate.return_type == "bool";
    } else if (is_ordered_csharp_operator(candidate.symbol)) {
      valid = arity == 2 && candidate.return_type == "bool";
    } else if ((candidate.symbol == "+" || candidate.symbol == "-") && (arity == 1 || arity == 2)) {
      valid = true;
    } else if (is_unary_csharp_operator(candidate.symbol)) {
      valid = arity == 1;
    } else if (is_binary_csharp_operator(candidate.symbol)) {
      valid = arity == 2;
    }
    if (!valid) {
      return;
    }

    string key = operator_signature_key(candidate.symbol, candidate);
    if (candidate_keys.insert(key).second) {
      candidates.push_back(std::move(candidate));
    }
  };

  for (Function *func : object->_methods) {
    if (func == nullptr) {
      continue;
    }

    string method_name = make_csharp_identifier(func->_ifunc.get_name());
    string symbol = get_csharp_operator_symbol(method_name);
    if (symbol.empty()) {
      continue;
    }

    for (FunctionRemap *remap : func->_remaps) {
      if (remap == nullptr ||
          (remap->_flags & FunctionRemap::F_explicit_self) != 0 ||
          remap->_type == FunctionRemap::T_constructor ||
          remap->_type == FunctionRemap::T_destructor ||
          remap->_void_return ||
          !remap->_has_this ||
          !is_remap_legal_csharp(remap)) {
        continue;
      }

      TypeIndex return_type_index = get_return_type_for_remap(remap);
      bool return_nullable = is_return_nullable(remap);
      string return_type = return_type_index != 0
        ? get_csharp_signature_type(return_type_index, return_nullable)
        : get_csharp_signature_type_for_wrapper(remap->_return_type, return_nullable);
      if (return_type.empty()) {
        continue;
      }

      CSharpOperatorCandidate candidate;
      candidate.method_name = method_name;
      candidate.symbol = symbol;
      candidate.return_type = return_type;
      candidate.parameters.push_back({declaring_type_name, "left", true, true});

      bool bad_param = false;
      for (size_t i = 1; i < remap->_parameters.size(); ++i) {
        TypeIndex param_type_index = get_parameter_type_for_remap(remap, i);
        bool param_nullable = is_parameter_nullable(remap, i);
        string param_type = param_type_index != 0
          ? get_csharp_signature_type(param_type_index, param_nullable, /*is_parameter=*/true)
          : get_csharp_signature_type_for_wrapper(remap->_parameters[i]._remap, param_nullable);

        if (csharp_stream_token_for_type(param_type_index) != AT_not_atomic) {
          bad_param = true;
          break;
        }
        if (param_type.empty()) {
          bad_param = true;
          break;
        }

        string param_name = get_csharp_parameter_name(remap, i);
        if (param_name == "left" || param_name == "value") {
          param_name = "param" + std::to_string(i - 1);
        }
        bool native_object = is_native_object_type(param_type_index);
        bool nullable_reference = native_object || !is_csharp_operator_value_type_name(param_type);
        candidate.parameters.push_back({param_type, param_name, nullable_reference, native_object});
      }
      if (bad_param) {
        continue;
      }

      add_candidate(std::move(candidate));
    }

    if (!func->_remaps.empty()) {
      continue;
    }

    int num_wrappers = func->_ifunc.number_of_c_wrappers();
    for (int wi = 0; wi < num_wrappers; ++wi) {
      FunctionWrapperIndex wrapper_index = func->_ifunc.get_c_wrapper(wi);
      if (wrapper_index == 0) {
        continue;
      }

      const InterrogateFunctionWrapper &wrapper = idb->get_wrapper(wrapper_index);
      if (wrapper.is_explicit_self() || func->_ifunc.is_constructor() ||
          func->_ifunc.is_destructor() || !is_wrapper_legal_csharp(wrapper) ||
          !wrapper.has_return_value()) {
        continue;
      }

      bool has_this = wrapper.number_of_parameters() != 0 && wrapper.parameter_is_this(0);
      if (!has_this) {
        continue;
      }

      TypeIndex return_type_index = wrapper.get_return_type();
      string return_type = get_csharp_signature_type(return_type_index, wrapper.is_return_nullable());
      if (return_type.empty()) {
        continue;
      }

      CSharpOperatorCandidate candidate;
      candidate.method_name = method_name;
      candidate.symbol = symbol;
      candidate.return_type = return_type;
      candidate.parameters.push_back({declaring_type_name, "left", true, true});

      bool bad_param = false;
      for (int i = 1; i < wrapper.number_of_parameters(); ++i) {
        TypeIndex param_type_index = wrapper.parameter_get_type(i);
        if (csharp_stream_token_for_type(param_type_index) != AT_not_atomic) {
          bad_param = true;
          break;
        }

        bool param_nullable = wrapper.parameter_is_nullable(i);
        string param_type = get_csharp_signature_type(param_type_index, param_nullable, /*is_parameter=*/true);
        if (param_type.empty()) {
          bad_param = true;
          break;
        }

        string param_name = wrapper.parameter_has_name(i)
          ? make_csharp_identifier(wrapper.parameter_get_name(i))
          : string("param") + std::to_string(i - 1);
        if (param_name == "left" || param_name == "value") {
          param_name = "param" + std::to_string(i - 1);
        }
        bool native_object = is_native_object_type(param_type_index);
        bool nullable_reference = native_object || !is_csharp_operator_value_type_name(param_type);
        candidate.parameters.push_back({param_type, param_name, nullable_reference, native_object});
      }
      if (bad_param) {
        continue;
      }

      add_candidate(std::move(candidate));
    }
  }

  if (candidates.empty()) {
    return;
  }

  std::map<string, const CSharpOperatorCandidate *> by_key;
  for (const CSharpOperatorCandidate &candidate : candidates) {
    by_key[operator_signature_key(candidate.symbol, candidate)] = &candidate;
  }

  std::set<string> local_emitted;
  std::set<string> &emitted =
    emitted_signatures != nullptr ? *emitted_signatures : local_emitted;

  auto emit_param_list = [&](const CSharpOperatorCandidate &candidate, bool equality) {
    for (size_t i = 0; i < candidate.parameters.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      const CSharpOperatorParam &param = candidate.parameters[i];
      string type = param.type;
      string name = param.name;
      if (i == 0) {
        name = candidate.parameters.size() == 1 ? "value" : "left";
        if (equality) {
          type += "?";
        }
      } else if (equality) {
        type = make_nullable_operator_type(param);
      }
      out << type << " " << name;
    }
  };

  auto emit_call_args = [&](const CSharpOperatorCandidate &candidate, bool suppress_nullable) {
    for (size_t i = 1; i < candidate.parameters.size(); ++i) {
      if (i != 1) {
        out << ", ";
      }
      out << candidate.parameters[i].name;
      if (suppress_nullable && candidate.parameters[i].nullable_reference) {
        out << "!";
      }
    }
  };

  auto emit_regular = [&](const CSharpOperatorCandidate &candidate, const string &symbol) {
    string key = "O:" + operator_signature_key(symbol, candidate);
    if (!emitted.insert(key).second) {
      return;
    }

    indent(out, indent_level) << "public static " << candidate.return_type
                              << " operator " << symbol << "(";
    emit_param_list(candidate, false);
    out << ") => ";
    string receiver = candidate.parameters.size() == 1 ? "value" : "left";
    out << receiver << "." << candidate.method_name << "(";
    emit_call_args(candidate, false);
    out << ");\n\n";
  };

  auto right_null_expression = [](const CSharpOperatorParam &param, const string &native_var) -> string {
    if (!param.nullable_reference) {
      return "false";
    }
    if (!param.native_object) {
      return param.name + " is null";
    }
    return "(" + param.name + " is null || " + param.name +
      " is INativeObject " + native_var + " && " + native_var + ".NativeHandle == IntPtr.Zero)";
  };

  auto emit_equality_overrides = [&](const std::vector<const CSharpOperatorCandidate *> &equality_candidates) {
    if (equality_candidates.empty() || !emitted.insert("O:EqualsOverride").second) {
      return;
    }

    std::vector<const CSharpOperatorCandidate *> equals_checks;
    std::set<string> equals_param_types;
    for (const CSharpOperatorCandidate *candidate : equality_candidates) {
      if (candidate == nullptr || candidate->parameters.size() != 2) {
        continue;
      }

      string param_type = strip_nullable_operator_type(candidate->parameters[1].type);
      if (param_type.empty()) {
        continue;
      }

      if (candidate->symbol == "==" && equals_param_types.insert(param_type).second) {
        equals_checks.push_back(candidate);
      }
    }
    for (const CSharpOperatorCandidate *candidate : equality_candidates) {
      if (candidate == nullptr || candidate->parameters.size() != 2) {
        continue;
      }

      string param_type = strip_nullable_operator_type(candidate->parameters[1].type);
      if (param_type.empty()) {
        continue;
      }

      if (candidate->symbol == "!=" && equals_param_types.insert(param_type).second) {
        equals_checks.push_back(candidate);
      }
    }

    if (equals_checks.empty()) {
      return;
    }

    indent(out, indent_level) << "public override bool Equals(object? obj) {\n";
    indent(out, indent_level + 2) << "if (ReferenceEquals(this, obj)) {\n";
    indent(out, indent_level + 4) << "return true;\n";
    indent(out, indent_level + 2) << "}\n";
    indent(out, indent_level + 2) << "if (NativeHandle == IntPtr.Zero) {\n";
    indent(out, indent_level + 4) << "return obj is INativeObject __p3objNativeNull && __p3objNativeNull.NativeHandle == IntPtr.Zero;\n";
    indent(out, indent_level + 2) << "}\n";
    indent(out, indent_level + 2) << "if (obj is INativeObject __p3objNative && __p3objNative.NativeHandle == IntPtr.Zero) {\n";
    indent(out, indent_level + 4) << "return false;\n";
    indent(out, indent_level + 2) << "}\n";

    for (size_t i = 0; i < equals_checks.size(); ++i) {
      const CSharpOperatorCandidate &candidate = *equals_checks[i];
      const CSharpOperatorParam &right = candidate.parameters[1];
      string param_type = strip_nullable_operator_type(right.type);
      string var_name = "__p3eqOther" + std::to_string(i);

      indent(out, indent_level + 2) << "if (obj is " << param_type << " " << var_name << ") {\n";
      indent(out, indent_level + 4) << "return ";
      if (candidate.symbol == "!=") {
        out << "!";
      }
      out << "this." << candidate.method_name << "(" << var_name << ");\n";
      indent(out, indent_level + 2) << "}\n";
    }

    indent(out, indent_level + 2) << "return false;\n";
    indent(out, indent_level) << "}\n";
    indent(out, indent_level) << "public override int GetHashCode() => typeof("
                              << class_name << ").GetHashCode();\n\n";
  };

  auto emit_equality_pair = [&](const CSharpOperatorCandidate &candidate) {
    string eq_key = "O:" + operator_signature_key("==", candidate);
    string ne_key = "O:" + operator_signature_key("!=", candidate);
    if (!emitted.insert(eq_key).second) {
      return;
    }
    emitted.insert(ne_key);

    const CSharpOperatorParam &right = candidate.parameters[1];
    string right_null_left_check = right_null_expression(right, "__p3rightNativeLeft");

    indent(out, indent_level) << "public static bool operator ==(";
    emit_param_list(candidate, true);
    out << ") {\n";
    indent(out, indent_level + 2) << "if (left is null || left.NativeHandle == IntPtr.Zero) {\n";
    indent(out, indent_level + 4) << "return " << right_null_left_check << ";\n";
    indent(out, indent_level + 2) << "}\n";
    indent(out, indent_level + 2) << "return left.Equals(" << right.name << ");\n";
    indent(out, indent_level) << "}\n";
    indent(out, indent_level) << "public static bool operator !=(";
    emit_param_list(candidate, true);
    out << ") => !(left == " << right.name << ");\n\n";

  };

  std::vector<const CSharpOperatorCandidate *> equality_candidates;
  for (const CSharpOperatorCandidate &candidate : candidates) {
    if (is_equality_csharp_operator(candidate.symbol)) {
      equality_candidates.push_back(&candidate);
    }
  }
  if (!is_interface) {
    emit_equality_overrides(equality_candidates);
  }

  for (const CSharpOperatorCandidate &candidate : candidates) {
    if (is_equality_csharp_operator(candidate.symbol)) {
      if (!is_interface) {
        emit_equality_pair(candidate);
      }
      continue;
    }

    if (is_ordered_csharp_operator(candidate.symbol)) {
      string opposite = opposite_ordered_operator(candidate.symbol);
      string pair_key = operator_signature_key(opposite, candidate);
      auto pair_it = by_key.find(pair_key);
      if (pair_it == by_key.end()) {
        continue;
      }
      if (candidate.symbol == "<" || candidate.symbol == "<=") {
        emit_regular(candidate, candidate.symbol);
        emit_regular(*pair_it->second, opposite);
      }
      continue;
    }

    emit_regular(candidate, candidate.symbol);
  }
}

/**
 *
 */
void InterfaceMakerCSharp::
write_method(ostream &out, Function *func, Object *object, int indent_level,
             bool is_interface, std::set<string> *emitted_signatures) {
  std::set<string> local_emitted_signatures;
  std::set<string> &signature_set =
    (emitted_signatures != nullptr) ? (*emitted_signatures) : local_emitted_signatures;
  string method_name = make_csharp_identifier(func->_ifunc.get_name());

  Function::Remaps::const_iterator ri;
  for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
    FunctionRemap *remap = (*ri);
    const InterrogateFunctionWrapper *wrapper = get_wrapper_for_remap(remap);
    if (
        (remap->_flags & FunctionRemap::F_explicit_self) != 0 ||
        remap->_type == FunctionRemap::T_constructor ||
        remap->_type == FunctionRemap::T_destructor ||
        !is_remap_legal_csharp(remap)) {
      continue;
    }

    bool is_static = !remap->_has_this;
    if (is_interface && is_static) {
      continue;
    }
    if (is_static && object != nullptr &&
        get_type_index_for_interrogate_type(func->_itype) != get_type_index_for_interrogate_type(object->_itype)) {
      continue;
    }

    TypeIndex return_type_index = get_return_type_for_remap(remap);
    bool return_nullable = is_return_nullable(remap);
    string return_type = remap->_void_return ? "void" :
      ((return_type_index != 0)
         ? get_csharp_signature_type(return_type_index, return_nullable)
         : get_csharp_signature_type_for_wrapper(remap->_return_type, return_nullable));


    std::vector<string> param_types;
    std::vector<string> param_decls;
    std::vector<string> param_names;
    std::vector<string> native_args;

    size_t first_param = remap->_has_this ? 1 : 0;
    if (remap->_has_this) {
      native_args.push_back(get_native_this_argument(object, func, remap));
    }

    // Collected stream-bridge allocations needed before the native call:
    // (bridge variable name, factory call, managed parameter name, nullable).
    struct StreamBridgeSite { string var; string factory; string param; bool nullable; };
    std::vector<StreamBridgeSite> stream_bridges;

    for (size_t i = first_param; i < remap->_parameters.size(); ++i) {
      ParameterRemap *param_remap = remap->_parameters[i]._remap;
      TypeIndex param_type_index = get_parameter_type_for_remap(remap, i);
      bool param_nullable = is_parameter_nullable(remap, i);
      string param_type = (param_type_index != 0)
        ? get_csharp_signature_type(param_type_index, param_nullable, /*is_parameter=*/true)
        : get_csharp_signature_type_for_wrapper(param_remap, param_nullable);

      // Stream parameters: override the default IntPtr mapping with
      // System.IO.Stream on the managed side — the StreamBridge emitted
      // below handles conversion back to the IntPtr the pinvoke needs.
      AtomicToken stream_tok = csharp_stream_token_for_type(param_type_index);
      if (stream_tok != AT_not_atomic) {
        param_type = "global::System.IO.Stream";
      }

      string param_name = get_csharp_parameter_name(remap, i);
      param_types.push_back(param_type);
      param_decls.push_back(param_type + " " + param_name);
      param_names.push_back(param_name);

      if (stream_tok != AT_not_atomic) {
        string bridge_var = "__p3stream" + std::to_string(stream_bridges.size());
        stream_bridges.push_back({bridge_var, csharp_stream_bridge_factory(stream_tok),
                                  param_name, param_nullable});
        native_args.push_back(bridge_var + ".Handle");
      } else {
        native_args.push_back(marshal_managed_argument(param_type_index, param_type, param_name));
      }
    }

    // Skip this overload if any type resolved to "" (meaning the underlying C++
    // type is not exported / is a skipped type such as a forward-declared class
    // that has no F_fully_defined flag).  Emitting IFoo for such a type would
    // produce a CS0246 compile error because no IFoo.cs is ever generated.
    if (return_type.empty()) {
      continue;
    }
    {
      bool has_skipped_param = false;
      for (const string &pt : param_types) {
        if (pt.empty()) { has_skipped_param = true; break; }
      }
      if (has_skipped_param) {
        continue;
      }
    }

    string signature_key = build_signature_key(method_name, param_types, is_static);
    if (!signature_set.insert("M:" + signature_key).second) {
      continue;
    }

    int inherited_kind = object != nullptr
      ? inherited_method_signature_kind(object->_itype, method_name, param_types, return_type, is_static,
                                        !is_interface)
      : 0;

    if (is_interface && inherited_kind != 0) {
      continue;
    }

    if (func->_ifunc.has_comment()) {
      emit_xml_doc_comment(out, func->_ifunc.get_comment(), indent_level);
    }

    indent(out, indent_level);
    if (!is_interface) {
      out << "public ";
      if (is_static) {
        if (inherited_kind != 0) {
          out << "new ";
        }
        out << "static ";
      } else if (inherited_kind == 2) {
        out << "override ";
      } else if (inherited_kind == 1) {
        out << "new ";
      } else {
        out << "virtual ";
      }
    }
    out << return_type << " " << method_name << "(";
    for (size_t i = 0; i < param_decls.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << param_decls[i];
    }
    out << ")";

    string alias_name = to_pascal_case(method_name);
    string alias_signature_key = build_signature_key(alias_name, param_types, is_static);
    int inherited_alias_kind = object != nullptr
      ? inherited_method_signature_kind(object->_itype, alias_name, param_types, return_type, is_static,
                                        !is_interface)
      : 0;

    if (is_interface) {
      out << ";\n";

      if (alias_name != method_name && alias_name != "Dispose" && !is_blocked_pascal_alias(alias_name) && inherited_alias_kind == 0 &&
          signature_set.find("N:" + alias_name) == signature_set.end() &&
          signature_set.find("R:" + alias_signature_key) == signature_set.end() &&
          signature_set.insert("A:" + alias_signature_key).second) {
        indent(out, indent_level) << "/// <inheritdoc cref=\"" << method_name << "\"/>\n";
        indent(out, indent_level) << return_type << " " << alias_name << "(";
        for (size_t i = 0; i < param_decls.size(); ++i) {
          if (i != 0) {
            out << ", ";
          }
          out << param_decls[i];
        }
        out << ");\n";
      }

      continue;
    }

    out << " {\n";

    // Emit `using var __p3streamN = StreamBridge.For…(paramN);` for each
    // stream parameter so the bridge is disposed as soon as the native call
    // returns (or throws).  Null params produce a null-handle bridge.
    for (const auto &b : stream_bridges) {
      indent(out, indent_level + 2) << "using var " << b.var << " = "
                                    << b.factory << "(" << b.param << ");\n";
    }

    string native_call = get_pinvoke_call_name(func, remap) + "(";
    for (size_t i = 0; i < native_args.size(); ++i) {
      if (i != 0) {
        native_call += ", ";
      }
      native_call += native_args[i];
    }
    native_call += ")";

    string concrete_return_type = (return_type_index != 0)
      ? get_csharp_native_object_class_name(return_type_index)
      : get_csharp_native_object_class_name(remap->_return_type->get_new_type());
    string managed_return_type = (return_type_index != 0)
      ? get_csharp_type(return_type_index, return_nullable)
      : get_csharp_type_for_wrapper(remap->_return_type, return_nullable);
    string pinvoke_return_type = (return_type_index != 0)
      ? get_pinvoke_type(return_type_index, true)
      : get_pinvoke_type(remap->_return_type->get_new_type(), true);
    if (remap->_void_return) {
      indent(out, indent_level + 2) << native_call << ";\n";

    } else if (managed_return_type == "string" || managed_return_type == "string?") {
      if (pinvoke_return_type == "string") {
        if (return_nullable) {
          indent(out, indent_level + 2) << "return " << native_call << ";\n";
        } else {
          indent(out, indent_level + 2) << "return " << native_call
                                        << " ?? throw new InvalidOperationException(\"Native method returned null.\");\n";
        }
      } else {
        indent(out, indent_level + 2) << "IntPtr result = " << native_call << ";\n";
        if (return_nullable) {
          indent(out, indent_level + 2)
            << "return result == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(result);\n";
        } else {
          indent(out, indent_level + 2)
            << "return result == IntPtr.Zero ? throw new InvalidOperationException(\"Native method returned null.\") : Marshal.PtrToStringUTF8(result)!;\n";
        }
      }

    } else if (!concrete_return_type.empty()) {
      indent(out, indent_level + 2) << "IntPtr result = " << native_call << ";\n";
      indent(out, indent_level + 2) << "return " << concrete_return_type
                                    << ".__CreateFromNative(result, "
                                    << ((wrapper != nullptr) ? get_native_ownership_name(*wrapper, false)
                                                             : get_native_ownership_name(remap, false))
                                    << ")";
      if (!return_nullable) {
        out << " ?? throw new InvalidOperationException(\"Native method returned null.\")";
      }
      out << ";\n";

    } else if (pinvoke_return_type != "IntPtr") {
      if (is_csharp_enum_type(return_type_index)) {
        indent(out, indent_level + 2) << "return (" << return_type << ")"
                                      << native_call << ";\n";
      } else {
        indent(out, indent_level + 2) << "return " << native_call << ";\n";
      }

    } else {
      indent(out, indent_level + 2) << "return " << native_call << ";\n";
    }

    indent(out, indent_level) << "}\n";

    string alias_call = is_static ? method_name : "this." + method_name;
    if (is_static) {
      string type_prefix = "global::" + prettify_namespace(_current_module_name) + ".";
      if (object != nullptr) {
        alias_call = type_prefix + get_nested_class_name(object->_itype) + "." + method_name;
      } else {
        alias_call = type_prefix + get_globals_class_name(_current_module_name) +
          "." + method_name;
      }
    }
    if (!is_interface && alias_name != method_name && alias_name != "Dispose" &&
        !is_blocked_pascal_alias(alias_name) &&
        signature_set.find("N:" + alias_name) == signature_set.end() &&
        signature_set.find("R:" + alias_signature_key) == signature_set.end() &&
        signature_set.insert("A:" + alias_signature_key).second) {
      indent(out, indent_level) << "/// <inheritdoc cref=\"" << method_name << "\"/>\n";
      indent(out, indent_level) << "public ";
      if (is_static) {
        if (inherited_alias_kind != 0) {
          out << "new ";
        }
        out << "static ";
      } else if (inherited_alias_kind == 2) {
        out << "override ";
      } else if (inherited_alias_kind == 1) {
        out << "new ";
      } else {
        out << "virtual ";
      }
      out << return_type << " " << alias_name << "(";
      for (size_t i = 0; i < param_decls.size(); ++i) {
        if (i != 0) {
          out << ", ";
        }
        out << param_decls[i];
      }
      out << ") => " << alias_call << "(";
      for (size_t i = 0; i < param_names.size(); ++i) {
        if (i != 0) {
          out << ", ";
        }
        // An out-parameter has to be forwarded as one.
        if (i < param_types.size() && param_types[i].compare(0, 4, "out ") == 0) {
          out << "out ";
        }
        out << param_names[i];
      }
      out << ");\n";
    }

    out << "\n";
  }

  if (!func->_remaps.empty()) {
    return;
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  int num_wrappers = func->_ifunc.number_of_c_wrappers();
  for (int wi = 0; wi < num_wrappers; ++wi) {
    FunctionWrapperIndex wrapper_index = func->_ifunc.get_c_wrapper(wi);
    if (wrapper_index == 0) {
      continue;
    }

    const InterrogateFunctionWrapper &wrapper = idb->get_wrapper(wrapper_index);
    if (wrapper.is_explicit_self() || func->_ifunc.is_constructor() ||
        func->_ifunc.is_destructor() || !is_wrapper_legal_csharp(wrapper)) {
      continue;
    }

    bool has_this = wrapper.number_of_parameters() != 0 && wrapper.parameter_is_this(0);
    bool is_static = !has_this;
    if (is_interface && is_static) {
      continue;
    }
    if (is_static && object != nullptr &&
        get_type_index_for_interrogate_type(func->_itype) != get_type_index_for_interrogate_type(object->_itype)) {
      continue;
    }

    TypeIndex return_type_index = wrapper.get_return_type();
    bool return_nullable = wrapper.is_return_nullable();
    string return_type = wrapper.has_return_value()
      ? get_csharp_signature_type(return_type_index, return_nullable) : "void";

    // A returned C++ stream is a real System.IO.Stream now (Interrogate.NativeStream
    // drives it in place).  It used to come back as an opaque IntPtr -- so
    // VirtualFile::open_read_file handed you something you could do nothing with,
    // and the only close method took the *parameter* path, bridging a brand-new
    // native stream and then double-freeing it.
    AtomicToken return_stream_tok = wrapper.has_return_value()
      ? csharp_stream_token_for_type(return_type_index) : AT_not_atomic;
    if (return_stream_tok != AT_not_atomic) {
      return_type = "global::System.IO.Stream?";
    }


    std::vector<string> param_types;
    std::vector<string> param_decls;
    std::vector<string> param_names;
    std::vector<string> native_args;

    int first_param = has_this ? 1 : 0;
    if (has_this && object != nullptr) {
      native_args.push_back(get_native_this_argument(object->_itype, func->_ifunc));
    }

    struct StreamBridgeSite { string var; string factory; string param; bool nullable; };
    std::vector<StreamBridgeSite> stream_bridges;

    for (int i = first_param; i < wrapper.number_of_parameters(); ++i) {
      TypeIndex param_type_index = wrapper.parameter_get_type(i);
      bool param_nullable = wrapper.parameter_is_nullable(i);
      string param_type = get_csharp_signature_type(param_type_index, param_nullable, /*is_parameter=*/true);

      AtomicToken stream_tok = csharp_stream_token_for_type(param_type_index);
      if (stream_tok != AT_not_atomic) {
        param_type = "global::System.IO.Stream";
      }

      // C++'s out-parameter idiom (a non-const reference to a number) marshals
      // straight through as `out T` -- no pinning, no unsafe.  The database says which
      // parameters those are; a raw `T *` buffer looks identical by now.
      string out_type = (stream_tok == AT_not_atomic && wrapper.parameter_is_out(i))
        ? csharp_out_parameter_type(param_type_index) : string();
      if (!out_type.empty()) {
        param_type = "out " + out_type;
      }

      string param_name = wrapper.parameter_has_name(i)
        ? make_csharp_identifier(wrapper.parameter_get_name(i))
        : string("param") + std::to_string(i - first_param);
      param_types.push_back(param_type);
      param_decls.push_back(param_type + " " + param_name);
      param_names.push_back(param_name);

      if (!out_type.empty()) {
        native_args.push_back("out " + param_name);
      } else if (stream_tok != AT_not_atomic) {
        string bridge_var = "__p3stream" + std::to_string(stream_bridges.size());
        stream_bridges.push_back({bridge_var, csharp_stream_bridge_factory(stream_tok),
                                  param_name, param_nullable});
        native_args.push_back(bridge_var + ".Handle");
      } else {
        native_args.push_back(marshal_managed_argument(param_type_index, param_type, param_name));
      }
    }

    // An empty signature type means the C# maker has no mapping for the C++
    // type (see should_skip_csharp_type), so the overload cannot be written.
    // Say which type and why -- a method that vanishes without a word is how
    // whole APIs go missing unnoticed.
    if (return_type.empty()) {
      record_skipped("method", func->_ifunc.get_scoped_name(),
                     "no C# mapping for return type '" +
                     report_type_name(return_type_index) + "'");
      continue;
    }
    {
      bool has_skipped_param = false;
      for (size_t pi = 0; pi < param_types.size(); ++pi) {
        if (param_types[pi].empty()) {
          record_skipped("method", func->_ifunc.get_scoped_name(),
                         "no C# mapping for parameter '" + param_names[pi] +
                         "' of type '" +
                         report_type_name(wrapper.parameter_get_type((int)pi + first_param)) +
                         "'");
          has_skipped_param = true;
          break;
        }
      }
      if (has_skipped_param) {
        continue;
      }
    }

    string signature_key = build_signature_key(method_name, param_types, is_static);
    if (!signature_set.insert("M:" + signature_key).second) {
      continue;
    }

    int inherited_kind = object != nullptr
      ? inherited_method_signature_kind(object->_itype, method_name, param_types, return_type, is_static,
                                        !is_interface)
      : 0;

    if (is_interface && inherited_kind != 0) {
      continue;
    }

    if (func->_ifunc.has_comment()) {
      emit_xml_doc_comment(out, func->_ifunc.get_comment(), indent_level);
    }

    indent(out, indent_level);
    if (!is_interface) {
      out << "public ";
      if (is_static) {
        if (inherited_kind != 0) {
          out << "new ";
        }
        out << "static ";
      } else if (inherited_kind == 2) {
        out << "override ";
      } else if (inherited_kind == 1) {
        out << "new ";
      } else {
        out << "virtual ";
      }
    }
    out << return_type << " " << method_name << "(";
    for (size_t i = 0; i < param_decls.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << param_decls[i];
    }
    out << ")";

    string alias_name = to_pascal_case(method_name);
    string alias_signature_key = build_signature_key(alias_name, param_types, is_static);
    int inherited_alias_kind = object != nullptr
      ? inherited_method_signature_kind(object->_itype, alias_name, param_types, return_type, is_static,
                                        !is_interface)
      : 0;

    if (is_interface) {
      out << ";\n";

      if (alias_name != method_name && alias_name != "Dispose" && !is_blocked_pascal_alias(alias_name) && inherited_alias_kind == 0 &&
          signature_set.find("N:" + alias_name) == signature_set.end() &&
          signature_set.find("R:" + alias_signature_key) == signature_set.end() &&
          signature_set.insert("A:" + alias_signature_key).second) {
        indent(out, indent_level) << "/// <inheritdoc cref=\"" << method_name << "\"/>\n";
        indent(out, indent_level) << return_type << " " << alias_name << "(";
        for (size_t i = 0; i < param_decls.size(); ++i) {
          if (i != 0) {
            out << ", ";
          }
          out << param_decls[i];
        }
        out << ");\n";
      }

      continue;
    }

    out << " {\n";

    // Emit stream-bridge `using var` bindings — see the matching block above
    // in the remap path.
    for (const auto &b : stream_bridges) {
      indent(out, indent_level + 2) << "using var " << b.var << " = "
                                    << b.factory << "(" << b.param << ");\n";
    }

    string native_call = get_pinvoke_call_name(func->_ifunc, wrapper) + "(";
    for (size_t i = 0; i < native_args.size(); ++i) {
      if (i != 0) {
        native_call += ", ";
      }
      native_call += native_args[i];
    }
    native_call += ")";

    string concrete_return_type = get_csharp_native_object_class_name(return_type_index);
    string managed_return_type = get_csharp_type(return_type_index, return_nullable);
    string pinvoke_return_type = get_pinvoke_type(return_type_index, true);

    if (!wrapper.has_return_value()) {
      indent(out, indent_level + 2) << native_call << ";\n";

    } else if (return_stream_tok != AT_not_atomic) {
      indent(out, indent_level + 2) << "IntPtr result = " << native_call << ";\n";
      indent(out, indent_level + 2) << "return global::Interrogate.NativeStream.Wrap(result, "
                                    << csharp_native_stream_kind(return_stream_tok) << ");\n";

    } else if (managed_return_type == "string" || managed_return_type == "string?") {
      if (pinvoke_return_type == "string") {
        if (return_nullable) {
          indent(out, indent_level + 2) << "return " << native_call << ";\n";
        } else {
          indent(out, indent_level + 2) << "return " << native_call
                                        << " ?? throw new InvalidOperationException(\"Native method returned null.\");\n";
        }
      } else {
        indent(out, indent_level + 2) << "IntPtr result = " << native_call << ";\n";
        if (return_nullable) {
          indent(out, indent_level + 2)
            << "return result == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(result);\n";
        } else {
          indent(out, indent_level + 2)
            << "return result == IntPtr.Zero ? throw new InvalidOperationException(\"Native method returned null.\") : Marshal.PtrToStringUTF8(result)!;\n";
        }
      }

    } else if (!concrete_return_type.empty()) {
      indent(out, indent_level + 2) << "IntPtr result = " << native_call << ";\n";
      indent(out, indent_level + 2) << "return " << concrete_return_type
                                    << ".__CreateFromNative(result, "
                                    << get_native_ownership_name(wrapper, false)
                                    << ")";
      if (!return_nullable) {
        out << " ?? throw new InvalidOperationException(\"Native method returned null.\")";
      }
      out << ";\n";

    } else if (pinvoke_return_type != "IntPtr") {
      if (is_csharp_enum_type(return_type_index)) {
        indent(out, indent_level + 2) << "return (" << return_type << ")"
                                      << native_call << ";\n";
      } else {
        indent(out, indent_level + 2) << "return " << native_call << ";\n";
      }

    } else {
      indent(out, indent_level + 2) << "return " << native_call << ";\n";
    }

    indent(out, indent_level) << "}\n";

    string alias_call = is_static ? method_name : "this." + method_name;
    if (is_static) {
      string type_prefix = "global::" + prettify_namespace(_current_module_name) + ".";
      if (object != nullptr) {
        alias_call = type_prefix + get_nested_class_name(object->_itype) + "." + method_name;
      } else {
        alias_call = type_prefix + get_globals_class_name(_current_module_name) +
          "." + method_name;
      }
    }
    if (alias_name != method_name && alias_name != "Dispose" &&
        !is_blocked_pascal_alias(alias_name) &&
        signature_set.find("N:" + alias_name) == signature_set.end() &&
        signature_set.find("R:" + alias_signature_key) == signature_set.end() &&
        signature_set.insert("A:" + alias_signature_key).second) {
      indent(out, indent_level) << "/// <inheritdoc cref=\"" << method_name << "\"/>\n";
      indent(out, indent_level) << "public ";
      if (is_static) {
        if (inherited_alias_kind != 0) {
          out << "new ";
        }
        out << "static ";
      } else if (inherited_alias_kind == 2) {
        out << "override ";
      } else if (inherited_alias_kind == 1) {
        out << "new ";
      } else {
        out << "virtual ";
      }
      out << return_type << " " << alias_name << "(";
      for (size_t i = 0; i < param_decls.size(); ++i) {
        if (i != 0) {
          out << ", ";
        }
        out << param_decls[i];
      }
      out << ") => " << alias_call << "(";
      for (size_t i = 0; i < param_names.size(); ++i) {
        if (i != 0) {
          out << ", ";
        }
        // An out-parameter has to be forwarded as one.
        if (i < param_types.size() && param_types[i].compare(0, 4, "out ") == 0) {
          out << "out ";
        }
        out << param_names[i];
      }
      out << ");\n";
    }

    out << "\n";
  }
}

/**
 *
 */
void InterfaceMakerCSharp::
write_property(ostream &out, Property *prop, Object *object, int indent_level,
               bool is_interface) {
  if (prop->_getter_remaps.empty() && prop->_setter_remaps.empty()) {
    return;
  }

  if (is_interface && !prop->_has_this) {
    return;
  }

  FunctionRemap *getter = prop->_getter_remaps.empty() ? nullptr : prop->_getter_remaps.front();
  FunctionRemap *setter = prop->_setter_remaps.empty() ? nullptr : prop->_setter_remaps.front();

  if (getter != nullptr && (getter->_void_return || !is_remap_legal_csharp(getter))) {
    getter = nullptr;
  }
  if (setter != nullptr && !is_remap_legal_csharp(setter)) {
    setter = nullptr;
  }
  if (getter == nullptr && setter == nullptr) {
    return;
  }

  size_t getter_params = getter != nullptr ? getter->_parameters.size() : 0;
  size_t setter_params = setter != nullptr ? setter->_parameters.size() : 0;
  size_t getter_extra = getter != nullptr && getter->_has_this ? 1 : 0;
  size_t setter_extra = setter != nullptr && setter->_has_this ? 1 : 0;
  if ((getter != nullptr && (getter_params - getter_extra) > 0) ||
      (setter != nullptr && (setter_params - setter_extra) > 1)) {
    return;
  }

  string property_type;
  if (getter != nullptr) {
    property_type = get_csharp_type_for_wrapper(getter->_return_type, is_return_nullable(getter));
  } else if (setter != nullptr && !setter->_parameters.empty()) {
    property_type = get_csharp_type_for_wrapper(setter->_parameters.back()._remap,
                                                is_parameter_nullable(setter, setter->_parameters.size() - 1));
  } else {
    return;
  }

  if (getter != nullptr && setter != nullptr && !setter->_parameters.empty()) {
    string setter_value_type = get_csharp_type_for_wrapper(setter->_parameters.back()._remap,
                                                           is_parameter_nullable(setter, setter->_parameters.size() - 1));
    if (setter_value_type != property_type) {
      setter = nullptr;
    }
  }

  string property_name = to_pascal_case(make_csharp_identifier(prop->_ielement.get_name()));
  if (property_name == "void" || property_type == "void") {
    return;
  }
  bool is_static = !prop->_has_this;

  indent(out, indent_level);
  if (!is_interface) {
    out << "public ";
    if (is_static) {
      out << "static ";
    }
  }
  out << property_type << " " << property_name;

  if (is_interface) {
    out << " {";
    if (getter != nullptr) {
      out << " get;";
    }
    if (setter != nullptr) {
      out << " set;";
    }
    out << " }\n";
    return;
  }

  out << " {\n";

  if (getter != nullptr) {
    indent(out, indent_level + 2) << "get {\n";
    Function *getter_func = nullptr;
    FunctionsByIndex::const_iterator gfi = _functions.find(prop->_ielement.get_getter());
    if (gfi != _functions.end()) {
      getter_func = (*gfi).second;
    }

    string native_call = get_pinvoke_call_name(getter_func, getter) + "(";
    if (getter->_has_this) {
      native_call += get_native_this_argument(object, getter_func, getter);
    }
    native_call += ")";

    CPPType *return_type_cpp = getter->_return_type->get_new_type();
    bool return_nullable = is_return_nullable(getter);
    if (TypeManager::is_bool(return_type_cpp) ||
        TypeManager::is_simple(return_type_cpp) ||
        TypeManager::is_enum(return_type_cpp)) {
      if (TypeManager::is_enum(return_type_cpp) && is_enum_type(_objects, return_type_cpp)) {
        indent(out, indent_level + 4) << "return (" << property_type << ")"
                                      << native_call << ";\n";
      } else {
        indent(out, indent_level + 4) << "return " << native_call << ";\n";
      }

    } else if (TypeManager::is_char_pointer(return_type_cpp) ||
               TypeManager::is_const_char_pointer(return_type_cpp) ||
               property_type == "string" || property_type == "string?") {
      string pinvoke_return_type = get_pinvoke_type(return_type_cpp, true);
      if (pinvoke_return_type == "string") {
        if (return_nullable) {
          indent(out, indent_level + 4) << "return " << native_call << ";\n";
        } else {
          indent(out, indent_level + 4) << "return " << native_call
                                        << " ?? throw new InvalidOperationException(\"Native getter returned null.\");\n";
        }
      } else {
        indent(out, indent_level + 4) << "IntPtr result = " << native_call << ";\n";
        if (return_nullable) {
          indent(out, indent_level + 4)
            << "return result == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(result);\n";
        } else {
          indent(out, indent_level + 4)
            << "return result == IntPtr.Zero ? throw new InvalidOperationException(\"Native getter returned null.\") : Marshal.PtrToStringUTF8(result)!;\n";
        }
      }

    } else if (is_csharp_native_object_type(_objects, return_type_cpp, property_type)) {
      indent(out, indent_level + 4) << "IntPtr result = " << native_call << ";\n";
      indent(out, indent_level + 4) << "return " << property_type
                                    << ".__CreateFromNative(result, "
                                    << get_native_ownership_name(getter, false)
                                    << ")";
      if (!return_nullable) {
        out << " ?? throw new InvalidOperationException(\"Native getter returned null.\")";
      }
      out << ";\n";

    } else {
      indent(out, indent_level + 4) << "return " << native_call << ";\n";
    }
    indent(out, indent_level + 2) << "}\n";
  }

  if (setter != nullptr) {
    indent(out, indent_level + 2) << "set {\n";
    Function *setter_func = nullptr;
    FunctionsByIndex::const_iterator sfi = _functions.find(prop->_ielement.get_setter());
    if (sfi != _functions.end()) {
      setter_func = (*sfi).second;
    }

    string native_call = get_pinvoke_call_name(setter_func, setter) + "(";
    bool need_comma = false;
    if (setter->_has_this) {
      native_call += get_native_this_argument(object, setter_func, setter);
      need_comma = true;
    }

    if (need_comma) {
      native_call += ", ";
    }
    if (!setter->_parameters.empty()) {
      ParameterRemap *value_remap = setter->_parameters.back()._remap;
      string value_type = get_csharp_type_for_wrapper(value_remap, false);
      TypeIndex value_type_index = get_type_index_for_cpp_type(value_remap->get_new_type());
      native_call += marshal_managed_argument(value_type_index, value_type, "value");
    }
    native_call += ")";

    indent(out, indent_level + 4) << native_call << ";\n";
    indent(out, indent_level + 2) << "}\n";
  }

  indent(out, indent_level) << "}\n\n";
}

/**
 * Emits a data-member property directly from the .in C-wrapper records, for the pass-2 (binary .in)
 * case where FunctionRemaps can't be rebuilt for interrogate-synthesized getters/setters (no
 * CPPInstance is available). This is the property analog of the wrapper-based method fallback, and it
 * is what lets public data members — including enum-typed ones — surface as C# properties.
 */
void InterfaceMakerCSharp::
write_property_from_wrapper(ostream &out, const InterrogateElement &ielement,
                            Object *object, int indent_level, bool is_interface) {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();

  // MAKE_SEQ_PROPERTY: the getter takes an index, so the simple-accessor path below
  // would discard it (and did -- silently, for all 107 of them).
  if (ielement.is_sequence()) {
    write_sequence_property_from_wrapper(out, ielement, object, indent_level,
                                         is_interface);
    return;
  }
  if (ielement.is_mapping()) {
    write_map_property_from_wrapper(out, ielement, object, indent_level, is_interface);
    return;
  }

  auto first_legal_wrapper = [&](FunctionIndex func_index, Function *&out_func)
      -> const InterrogateFunctionWrapper * {
    out_func = nullptr;
    if (func_index == 0) return nullptr;
    FunctionsByIndex::const_iterator it = _functions.find(func_index);
    if (it == _functions.end()) return nullptr;
    out_func = it->second;
    int n = out_func->_ifunc.number_of_c_wrappers();
    for (int wi = 0; wi < n; ++wi) {
      FunctionWrapperIndex widx = out_func->_ifunc.get_c_wrapper(wi);
      if (widx == 0) continue;
      const InterrogateFunctionWrapper &w = idb->get_wrapper(widx);
      if (w.is_explicit_self() || !is_wrapper_legal_csharp(w)) continue;
      return &w;
    }
    return nullptr;
  };

  Function *getter_func = nullptr;
  Function *setter_func = nullptr;
  Function *has_func = nullptr;
  const InterrogateFunctionWrapper *getter_w =
    ielement.has_getter() ? first_legal_wrapper(ielement.get_getter(), getter_func) : nullptr;
  const InterrogateFunctionWrapper *setter_w =
    ielement.has_setter() ? first_legal_wrapper(ielement.get_setter(), setter_func) : nullptr;

  // MAKE_PROPERTY2 pairs the getter with a has_xxx() predicate, and the getter is
  // only meaningful when it returns true (InputDevice::get_tracker is guarded by
  // has_tracker).  Model that as a nullable property -- null meaning "not present"
  // -- instead of calling the getter unconditionally and hoping.
  const InterrogateFunctionWrapper *has_w =
    ielement.has_has_function() ? first_legal_wrapper(ielement.get_has_function(), has_func) : nullptr;
  if (has_w != nullptr) {
    int hp = has_w->number_of_parameters();
    int hfirst = (hp != 0 && has_w->parameter_is_this(0)) ? 1 : 0;
    // It must take only `this` AND actually return bool.  Some elements record a
    // has-function that is really the getter again (PGScrollFrame's slider
    // properties do), and calling `if (!ptr)` on that does not compile.
    if (hp - hfirst != 0 ||
        !has_w->has_return_value() ||
        get_pinvoke_type(has_w->get_return_type(), true) != "bool") {
      has_w = nullptr;
    }
  }

  // Only simple accessors become properties: the getter takes just `this`, and the setter takes
  // `this` + exactly one value. Indexed/sequence accessors (extra parameters) are handled by the
  // indexer/MakeSeq machinery, not here.
  if (getter_w != nullptr) {
    int gp = getter_w->number_of_parameters();
    int gfirst = (gp != 0 && getter_w->parameter_is_this(0)) ? 1 : 0;
    if (gp - gfirst != 0) getter_w = nullptr;
  }
  if (setter_w != nullptr) {
    int sp = setter_w->number_of_parameters();
    int sfirst = (sp != 0 && setter_w->parameter_is_this(0)) ? 1 : 0;
    if (sp - sfirst != 1) setter_w = nullptr;
  }

  if (getter_w == nullptr && setter_w == nullptr) {
    return;
  }

  bool has_this;
  if (getter_w != nullptr) {
    has_this = getter_w->number_of_parameters() != 0 && getter_w->parameter_is_this(0);
  } else {
    has_this = setter_w->number_of_parameters() != 0 && setter_w->parameter_is_this(0);
  }
  // We can only reach the native "this" through an Object.
  if (has_this && object == nullptr) {
    return;
  }
  if (is_interface && !has_this) {
    return;
  }

  // Determine the property type from the getter's return, falling back to the setter's value.
  string property_type;
  TypeIndex ret_idx = 0;
  bool ret_nullable = false;
  if (getter_w != nullptr) {
    if (!getter_w->has_return_value()) {
      getter_w = nullptr;
    } else {
      ret_idx = getter_w->get_return_type();
      ret_nullable = getter_w->is_return_nullable();
      // Concrete type (get_qualified_class_name) — this is what a native-object property's
      // __CreateFromNative is a static member of; the interface (get_csharp_signature_type) has none.
      property_type = get_csharp_type(ret_idx, true);
    }
  }

  TypeIndex value_idx = 0;
  if (setter_w != nullptr) {
    int vparam = (setter_w->number_of_parameters() != 0 && setter_w->parameter_is_this(0)) ? 1 : 0;
    if (vparam < setter_w->number_of_parameters()) {
      value_idx = setter_w->parameter_get_type(vparam);
      string value_type = get_csharp_type(value_idx, true);
      if (property_type.empty()) {
        property_type = value_type;
      } else if (value_type != property_type) {
        setter_w = nullptr;   // getter/setter type mismatch — emit read-only.
      }
    } else {
      setter_w = nullptr;
    }
  }

  if (property_type.empty() || property_type == "void") {
    return;
  }

  TypeIndex primary_idx = (getter_w != nullptr) ? ret_idx : value_idx;
  bool is_value_type = is_csharp_enum_type(primary_idx)
                    || is_csharp_primitive_type(property_type)
                    || property_type == "bool"
                    || property_type == "string" || property_type == "string?";

  // A native-object property is marshalled exactly as a native-object return is
  // (__CreateFromNative with the wrapper's ownership) -- there is nothing special
  // about it, and refusing it dropped the property silently.  That refusal is why
  // only 98 of panda3d's ~1477 MAKE_PROPERTY declarations reached C#, and why the
  // whole gamepad surface (InputDevice.Tracker / .Battery) was missing: their
  // accessors are not PUBLISHED, so the property was the only way in.
  string concrete_type = get_csharp_native_object_class_name(primary_idx);

  if (!is_value_type && concrete_type.empty()) {
    record_skipped("property", ielement.get_scoped_name(),
                   "no C# mapping for property type '" +
                   report_type_name(primary_idx) + "'");
    return;
  }

  // Naming a class is not the same as that class existing: Event::get_receiver
  // returns EventReceiver, which is never emitted.  Use the same test the method
  // writer uses -- an empty signature type means "no C# mapping".
  if (!concrete_type.empty() &&
      get_csharp_signature_type(primary_idx, true).empty()) {
    record_skipped("property", ielement.get_scoped_name(),
                   "no C# mapping for property type '" +
                   report_type_name(primary_idx) + "'");
    return;
  }

  // Honour the has_xxx() guard only for object-typed properties, whose C# type is
  // already nullable.  Doing it for a value type would have to change the declared
  // type (bool -> bool?), and that breaks interface conformance where a derived
  // class publishes the same property unguarded: ITextProperties.SmallCaps is
  // bool, TextNode.SmallCaps would become bool?.  Value-typed MAKE_PROPERTY2 keeps
  // its existing unguarded behaviour.
  if (concrete_type.empty()) {
    has_w = nullptr;
  }

  string property_name = to_pascal_case(make_csharp_identifier(ielement.get_name()));
  if (property_name.empty() || property_name == "void") {
    return;
  }
  if (is_blocked_pascal_alias(property_name)) {
    return;
  }

  // C# forbids a member and a nested type sharing a name (CS0102).  InputDevice
  // has a nested BatteryData *and* a battery_data property; emitting both does not
  // compile.  The nested type is the one callers name in signatures, so it wins.
  if (object != nullptr) {
    const InterrogateType &owner = object->_itype;
    for (int i = 0; i < owner.number_of_nested_types(); ++i) {
      TypeIndex nested_index = owner.get_nested_type(i);
      if (nested_index == 0) {
        continue;
      }
      const InterrogateType &nested = idb->get_type(nested_index);
      if (should_nest_type(nested) &&
          get_simple_class_name(nested) == property_name) {
        record_skipped("property", ielement.get_scoped_name(),
                       "name collides with nested type '" + property_name +
                       "' (CS0102)");
        return;
      }
    }
  }

  bool is_static = !has_this;

  if (ielement.has_comment()) {
    emit_xml_doc_comment(out, ielement.get_comment(), indent_level);
  }

  indent(out, indent_level);
  if (!is_interface) {
    out << "public ";
    if (is_static) {
      out << "static ";
    }
  }
  out << property_type << " " << property_name;

  if (is_interface) {
    out << " {";
    if (getter_w != nullptr) out << " get;";
    if (setter_w != nullptr) out << " set;";
    out << " }\n";
    return;
  }

  out << " {\n";

  if (getter_w != nullptr) {
    indent(out, indent_level + 2) << "get {\n";
    string this_arg = has_this ? get_native_this_argument(object->_itype, getter_func->_ifunc) : "";
    string native_call = get_pinvoke_call_name(getter_func->_ifunc, *getter_w) + "(" + this_arg + ")";

    if (has_w != nullptr) {
      string has_this_arg = has_this
        ? get_native_this_argument(object->_itype, has_func->_ifunc) : "";
      indent(out, indent_level + 4)
        << "if (!" << get_pinvoke_call_name(has_func->_ifunc, *has_w)
        << "(" << has_this_arg << ")) return null;\n";
    }

    if (!concrete_type.empty()) {
      indent(out, indent_level + 4) << "IntPtr result = " << native_call << ";\n";
      indent(out, indent_level + 4) << "return " << concrete_type
                                    << ".__CreateFromNative(result, "
                                    << get_native_ownership_name(*getter_w, false)
                                    << ");\n";
    } else if (is_csharp_enum_type(ret_idx)) {
      indent(out, indent_level + 4) << "return (" << property_type << ")" << native_call << ";\n";
    } else if (property_type == "string" || property_type == "string?") {
      // The P/Invoke may marshal the string itself (returns string) or return a raw IntPtr.
      if (get_pinvoke_type(ret_idx, true) == "string") {
        if (ret_nullable) {
          indent(out, indent_level + 4) << "return " << native_call << ";\n";
        } else {
          indent(out, indent_level + 4) << "return " << native_call
                                        << " ?? throw new InvalidOperationException(\"Native getter returned null.\");\n";
        }
      } else {
        indent(out, indent_level + 4) << "IntPtr result = " << native_call << ";\n";
        if (ret_nullable) {
          indent(out, indent_level + 4) << "return result == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(result);\n";
        } else {
          indent(out, indent_level + 4) << "return result == IntPtr.Zero ? throw new InvalidOperationException(\"Native getter returned null.\") : Marshal.PtrToStringUTF8(result)!;\n";
        }
      }
    } else {
      indent(out, indent_level + 4) << "return " << native_call << ";\n";
    }
    indent(out, indent_level + 2) << "}\n";
  }

  if (setter_w != nullptr) {
    indent(out, indent_level + 2) << "set {\n";
    string this_arg = has_this ? get_native_this_argument(object->_itype, setter_func->_ifunc) : "";
    string value_type = get_csharp_type(value_idx, false);
    string native_call = get_pinvoke_call_name(setter_func->_ifunc, *setter_w) + "(";
    if (has_this) {
      native_call += this_arg + ", ";
    }
    native_call += marshal_managed_argument(value_idx, value_type, "value") + ")";
    indent(out, indent_level + 4) << native_call << ";\n";
    indent(out, indent_level + 2) << "}\n";
  }

  indent(out, indent_level) << "}\n\n";
}

/**
 * Emits a MAKE_SEQ_PROPERTY as an Interrogate.NativeSeq<T> -- a live IReadOnlyList<T>
 * over the (length, element) accessor pair.
 *
 * There is no native container to wrap here: the two accessors ARE the sequence, and
 * they are often the only way in at all.  InputDevice's get_num_axes / get_axis are not
 * PUBLISHED -- MAKE_SEQ_PROPERTY(axes, ...) is -- so with this unimplemented the whole
 * axis and button surface was unreachable from C#, and silently so.
 */
void InterfaceMakerCSharp::
write_sequence_property_from_wrapper(ostream &out, const InterrogateElement &ielement,
                                     Object *object, int indent_level, bool is_interface) {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();

  if (object == nullptr) {
    return;
  }

  // Find the wrapper with exactly `want_arity` parameters after `this`.  Taking the
  // first legal one is wrong: interrogate emits one wrapper per default-argument
  // count, and the first is the widest.  NodePath::get_node(int, Thread * = ...)
  // has wrappers of arity 2 and 1 -- only the second is the element accessor.
  auto find_wrapper = [&](FunctionIndex func_index, int want_arity, Function *&out_func)
      -> const InterrogateFunctionWrapper * {
    out_func = nullptr;
    if (func_index == 0) return nullptr;
    FunctionsByIndex::const_iterator it = _functions.find(func_index);
    if (it == _functions.end()) return nullptr;
    Function *func = it->second;
    int n = func->_ifunc.number_of_c_wrappers();
    for (int wi = 0; wi < n; ++wi) {
      FunctionWrapperIndex widx = func->_ifunc.get_c_wrapper(wi);
      if (widx == 0) continue;
      const InterrogateFunctionWrapper &w = idb->get_wrapper(widx);
      if (w.is_explicit_self() || !is_wrapper_legal_csharp(w)) continue;
      int np = w.number_of_parameters();
      bool has_this = np != 0 && w.parameter_is_this(0);
      if (np - (has_this ? 1 : 0) != want_arity) continue;
      out_func = func;
      return &w;
    }
    return nullptr;
  };

  auto is_static_wrapper = [](const InterrogateFunctionWrapper &w) {
    return w.number_of_parameters() == 0 || !w.parameter_is_this(0);
  };

  Function *length_func = nullptr;
  Function *getter_func = nullptr;
  Function *setter_func = nullptr;
  const InterrogateFunctionWrapper *length_w =
    find_wrapper(ielement.get_length_function(), 0, length_func);
  const InterrogateFunctionWrapper *getter_w =
    ielement.has_getter() ? find_wrapper(ielement.get_getter(), 1, getter_func) : nullptr;
  const InterrogateFunctionWrapper *setter_w =
    ielement.has_setter() ? find_wrapper(ielement.get_setter(), 2, setter_func) : nullptr;

  if (length_w == nullptr || getter_w == nullptr || !getter_w->has_return_value()) {
    record_skipped("seq-property", ielement.get_scoped_name(),
                   "no accessor pair of the shape (length) / (index)");
    return;
  }

  // The two need not agree on staticness: LMatrix declares
  // MAKE_SEQ_PROPERTY(rows, size, get_row) where size() is static and get_row() is
  // not.  The element accessor decides what the property is -- a static getter has
  // no instance to read through -- and a static length is simply called without one.
  bool getter_is_static = is_static_wrapper(*getter_w);
  bool length_is_static = is_static_wrapper(*length_w);
  if (getter_is_static && !length_is_static) {
    record_skipped("seq-property", ielement.get_scoped_name(),
                   "static element accessor with an instance length accessor");
    return;
  }
  if (setter_w != nullptr && is_static_wrapper(*setter_w) != getter_is_static) {
    setter_w = nullptr;
  }
  if (is_interface && getter_is_static) {
    return;
  }

  TypeIndex element_index = getter_w->get_return_type();
  string element_type = get_csharp_signature_type(element_index, false);
  if (element_type.empty()) {
    record_skipped("seq-property", ielement.get_scoped_name(),
                   "no C# mapping for element type '" +
                   report_type_name(element_index) + "'");
    return;
  }
  string concrete_type = get_csharp_native_object_class_name(element_index);

  string property_name = to_pascal_case(make_csharp_identifier(ielement.get_name()));
  if (property_name.empty() || property_name == "void" ||
      is_blocked_pascal_alias(property_name)) {
    return;
  }

  // A member may not share a name with a nested type (CS0102).
  {
    const InterrogateType &owner = object->_itype;
    for (int i = 0; i < owner.number_of_nested_types(); ++i) {
      TypeIndex nested_index = owner.get_nested_type(i);
      if (nested_index == 0) continue;
      const InterrogateType &nested = idb->get_type(nested_index);
      if (should_nest_type(nested) &&
          get_simple_class_name(nested) == property_name) {
        record_skipped("seq-property", ielement.get_scoped_name(),
                       "name collides with nested type '" + property_name + "' (CS0102)");
        return;
      }
    }
  }

  string seq_type = "global::Interrogate.NativeSeq<" + element_type + ">";

  if (ielement.has_comment()) {
    emit_xml_doc_comment(out, ielement.get_comment(), indent_level);
  }

  indent(out, indent_level);
  if (!is_interface) {
    out << "public ";
    if (getter_is_static) {
      out << "static ";
    }
  }
  out << seq_type << " " << property_name;

  if (is_interface) {
    out << " { get; }\n";
    return;
  }

  out << " {\n";
  indent(out, indent_level + 2) << "get {\n";

  string length_this = length_is_static
    ? string() : get_native_this_argument(object->_itype, length_func->_ifunc);
  string getter_this = getter_is_static
    ? string() : get_native_this_argument(object->_itype, getter_func->_ifunc);
  string getter_this_arg = getter_this.empty() ? string() : getter_this + ", ";

  // The index parameter is size_t in C++ far more often than int, so cast rather
  // than assume the P/Invoke takes an int.
  string index_cast =
    get_pinvoke_type(getter_w->parameter_get_type(getter_is_static ? 0 : 1), false);
  string index_arg = "(" + index_cast + ")__index";

  string element_call =
    get_pinvoke_call_name(getter_func->_ifunc, *getter_w) +
    "(" + getter_this_arg + index_arg + ")";

  string element_expr;
  if (!concrete_type.empty()) {
    element_expr = concrete_type + ".__CreateFromNative(" + element_call + ", " +
                   get_native_ownership_name(*getter_w, false) + ")!";
  } else if (is_csharp_enum_type(element_index)) {
    element_expr = "(" + element_type + ")" + element_call;
  } else if ((element_type == "string" || element_type == "string?") &&
             get_pinvoke_type(element_index, true) != "string") {
    element_expr = "Marshal.PtrToStringUTF8(" + element_call + ") ?? string.Empty";
  } else {
    element_expr = element_call;
  }

  indent(out, indent_level + 4) << "return new " << seq_type << "(\n";
  indent(out, indent_level + 6) << "() => (int)"
                                << get_pinvoke_call_name(length_func->_ifunc, *length_w)
                                << "(" << length_this << "),\n";
  indent(out, indent_level + 6) << "__index => " << element_expr;

  if (setter_w != nullptr) {
    TypeIndex value_index = setter_w->parameter_get_type(getter_is_static ? 1 : 2);
    string value_type = get_csharp_type(value_index, false);
    string setter_this = getter_is_static
      ? string() : get_native_this_argument(object->_itype, setter_func->_ifunc);
    string setter_this_arg = setter_this.empty() ? string() : setter_this + ", ";
    string setter_index_cast = get_pinvoke_type(setter_w->parameter_get_type(setter_this.empty() ? 0 : 1), false);

    out << ",\n";
    indent(out, indent_level + 6) << "(__index, __value) => "
                                  << get_pinvoke_call_name(setter_func->_ifunc, *setter_w)
                                  << "(" << setter_this_arg << "("
                                  << setter_index_cast << ")__index, "
                                  << marshal_managed_argument(value_index, value_type, "__value")
                                  << ")";
  }
  out << ");\n";

  indent(out, indent_level + 2) << "}\n";
  indent(out, indent_level) << "}\n\n";
}

/**
 * Emits a MAKE_MAP_PROPERTY as an Interrogate.NativeLookup<TKey, TValue>, or as a
 * NativeMap<TKey, TValue> (a real IReadOnlyDictionary) when the class also declared
 * a MAKE_MAP_KEYS_SEQ and the keys are therefore reachable.
 *
 * Not every map property is enumerable: RenderState::attribs declares only
 * has_attrib and get_attrib, and there is simply no way to ask it for its keys.
 * PandaNode::tags pairs MAKE_MAP_PROPERTY with MAKE_MAP_KEYS_SEQ(tags, get_num_tags,
 * get_tag_key) and so is.  Offering a Count that cannot be computed would be worse
 * than not offering one.
 */
void InterfaceMakerCSharp::
write_map_property_from_wrapper(ostream &out, const InterrogateElement &ielement,
                                Object *object, int indent_level, bool is_interface) {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();

  if (object == nullptr) {
    return;
  }

  auto find_wrapper = [&](FunctionIndex func_index, int want_arity, Function *&out_func)
      -> const InterrogateFunctionWrapper * {
    out_func = nullptr;
    if (func_index == 0) return nullptr;
    FunctionsByIndex::const_iterator it = _functions.find(func_index);
    if (it == _functions.end()) return nullptr;
    Function *func = it->second;
    int n = func->_ifunc.number_of_c_wrappers();
    for (int wi = 0; wi < n; ++wi) {
      FunctionWrapperIndex widx = func->_ifunc.get_c_wrapper(wi);
      if (widx == 0) continue;
      const InterrogateFunctionWrapper &w = idb->get_wrapper(widx);
      if (w.is_explicit_self() || !is_wrapper_legal_csharp(w)) continue;
      int np = w.number_of_parameters();
      if (np == 0 || !w.parameter_is_this(0)) continue;   // instance accessors only
      if (np - 1 != want_arity) continue;
      out_func = func;
      return &w;
    }
    return nullptr;
  };

  Function *has_func = nullptr;
  Function *get_func = nullptr;
  Function *set_func = nullptr;
  Function *del_func = nullptr;
  Function *len_func = nullptr;
  Function *key_func = nullptr;

  const InterrogateFunctionWrapper *has_w =
    ielement.has_has_function() ? find_wrapper(ielement.get_has_function(), 1, has_func) : nullptr;
  const InterrogateFunctionWrapper *get_w =
    ielement.has_getter() ? find_wrapper(ielement.get_getter(), 1, get_func) : nullptr;
  const InterrogateFunctionWrapper *set_w =
    ielement.has_setter() ? find_wrapper(ielement.get_setter(), 2, set_func) : nullptr;
  // Per-key removal is the del function (MAKE_MAP_PROPERTY's 5th argument, clear_tag);
  // the clear function is the separate clear-everything.  They are different slots.
  Function *clear_func = nullptr;
  const InterrogateFunctionWrapper *del_w =
    ielement.has_del_function() ? find_wrapper(ielement.get_del_function(), 1, del_func) : nullptr;
  const InterrogateFunctionWrapper *clear_w =
    ielement.has_clear_function() ? find_wrapper(ielement.get_clear_function(), 0, clear_func) : nullptr;
  const InterrogateFunctionWrapper *len_w =
    find_wrapper(ielement.get_length_function(), 0, len_func);
  const InterrogateFunctionWrapper *key_w =
    ielement.has_getkey_function() ? find_wrapper(ielement.get_getkey_function(), 1, key_func) : nullptr;

  if (has_w == nullptr || get_w == nullptr || !get_w->has_return_value()) {
    record_skipped("map-property", ielement.get_scoped_name(),
                   "no (this, key) has/get accessor pair");
    return;
  }
  // The predicate must be one.  Camera::aux_scene_data reuses its getter as the
  // has-function, and `if (!ptr)` is not a bool.
  if (!has_w->has_return_value() ||
      get_pinvoke_type(has_w->get_return_type(), true) != "bool") {
    record_skipped("map-property", ielement.get_scoped_name(),
                   "has-function is not a bool predicate");
    return;
  }

  TypeIndex key_index = get_w->parameter_get_type(1);
  TypeIndex value_index = get_w->get_return_type();

  string key_type = get_csharp_signature_type(key_index, false, /*is_parameter=*/true);
  string value_type = get_csharp_signature_type(value_index, false);
  if (key_type.empty() || value_type.empty()) {
    record_skipped("map-property", ielement.get_scoped_name(),
                   "no C# mapping for key '" + report_type_name(key_index) +
                   "' or value '" + report_type_name(value_index) + "'");
    return;
  }

  string property_name = to_pascal_case(make_csharp_identifier(ielement.get_name()));
  if (property_name.empty() || property_name == "void" ||
      is_blocked_pascal_alias(property_name)) {
    return;
  }
  {
    const InterrogateType &owner = object->_itype;
    for (int i = 0; i < owner.number_of_nested_types(); ++i) {
      TypeIndex nested_index = owner.get_nested_type(i);
      if (nested_index == 0) continue;
      const InterrogateType &nested = idb->get_type(nested_index);
      if (should_nest_type(nested) &&
          get_simple_class_name(nested) == property_name) {
        record_skipped("map-property", ielement.get_scoped_name(),
                       "name collides with nested type '" + property_name + "' (CS0102)");
        return;
      }
    }
  }

  // Four shapes, and the type says which one you have rather than throwing when you
  // find out.  Keys are reachable only with a MAKE_MAP_KEYS_SEQ (RenderState::attribs
  // has none), and the property is writable only if it declared a setter
  // (GeomVertexFormat::columns declares none).
  bool enumerable = (len_w != nullptr && key_w != nullptr && key_w->has_return_value());
  bool writable = (set_w != nullptr);

  const char *map_class = enumerable
    ? (writable ? "NativeMap<" : "NativeReadOnlyMap<")
    : (writable ? "NativeMutableLookup<" : "NativeLookup<");
  string map_type = string("global::Interrogate.") + map_class +
                    key_type + ", " + value_type + ">";

  if (ielement.has_comment()) {
    emit_xml_doc_comment(out, ielement.get_comment(), indent_level);
  }

  indent(out, indent_level);
  if (!is_interface) {
    out << "public ";
  }
  out << map_type << " " << property_name;

  if (is_interface) {
    out << " { get; }\n";
    return;
  }

  out << " {\n";
  indent(out, indent_level + 2) << "get {\n";

  // Converts a native return into the managed value/key it stands for.
  auto managed_expr = [&](TypeIndex type_index, const string &managed_type,
                          const InterrogateFunctionWrapper &w,
                          const string &call) -> string {
    string concrete = get_csharp_native_object_class_name(type_index);
    if (!concrete.empty()) {
      return concrete + ".__CreateFromNative(" + call + ", " +
             get_native_ownership_name(w, false) + ")!";
    }
    if (is_csharp_enum_type(type_index)) {
      return "(" + managed_type + ")" + call;
    }
    if ((managed_type == "string" || managed_type == "string?") &&
        get_pinvoke_type(type_index, true) != "string") {
      return "Marshal.PtrToStringUTF8(" + call + ") ?? string.Empty";
    }
    return call;
  };

  string has_this = get_native_this_argument(object->_itype, has_func->_ifunc);
  string get_this = get_native_this_argument(object->_itype, get_func->_ifunc);
  string key_arg = marshal_managed_argument(key_index, key_type, "__key");

  string has_call = get_pinvoke_call_name(has_func->_ifunc, *has_w) +
    "(" + has_this + ", " + marshal_managed_argument(has_w->parameter_get_type(1), key_type, "__key") + ")";
  string get_call = get_pinvoke_call_name(get_func->_ifunc, *get_w) +
    "(" + get_this + ", " + key_arg + ")";

  indent(out, indent_level + 4) << "return new " << map_type << "(\n";
  indent(out, indent_level + 6) << "__key => " << has_call << ",\n";
  indent(out, indent_level + 6) << "__key => "
                                << managed_expr(value_index, value_type, *get_w, get_call);

  if (enumerable) {
    string len_this = get_native_this_argument(object->_itype, len_func->_ifunc);
    string key_this = get_native_this_argument(object->_itype, key_func->_ifunc);
    TypeIndex key_return = key_w->get_return_type();
    string index_cast = get_pinvoke_type(key_w->parameter_get_type(1), false);
    string key_call = get_pinvoke_call_name(key_func->_ifunc, *key_w) +
      "(" + key_this + ", (" + index_cast + ")__index)";

    out << ",\n";
    indent(out, indent_level + 6) << "() => (int)"
                                  << get_pinvoke_call_name(len_func->_ifunc, *len_w)
                                  << "(" << len_this << "),\n";
    indent(out, indent_level + 6) << "__index => "
                                  << managed_expr(key_return, key_type, *key_w, key_call);
  }

  // The mutators only exist on the writable types, so nothing here can emit a lambda
  // the constructor has no slot for.
  if (writable) {
    TypeIndex set_value_index = set_w->parameter_get_type(2);
    string set_value_type = get_csharp_type(set_value_index, false);
    string set_this = get_native_this_argument(object->_itype, set_func->_ifunc);
    out << ",\n";
    indent(out, indent_level + 6) << "(__key, __value) => "
                                  << get_pinvoke_call_name(set_func->_ifunc, *set_w)
                                  << "(" << set_this << ", "
                                  << marshal_managed_argument(set_w->parameter_get_type(1), key_type, "__key")
                                  << ", "
                                  << marshal_managed_argument(set_value_index, set_value_type, "__value")
                                  << ")";

    if (del_w != nullptr) {
      string del_this = get_native_this_argument(object->_itype, del_func->_ifunc);
      out << ",\n";
      indent(out, indent_level + 6) << "__key => "
                                    << get_pinvoke_call_name(del_func->_ifunc, *del_w)
                                    << "(" << del_this << ", "
                                    << marshal_managed_argument(del_w->parameter_get_type(1), key_type, "__key")
                                    << ")";
    } else if (enumerable && clear_w != nullptr) {
      out << ",\n";
      indent(out, indent_level + 6) << "null";
    }

    // Clear-all is a NativeMap notion; a lookup has no "everything".
    if (enumerable && clear_w != nullptr) {
      string clear_this = get_native_this_argument(object->_itype, clear_func->_ifunc);
      out << ",\n";
      indent(out, indent_level + 6) << "() => "
                                    << get_pinvoke_call_name(clear_func->_ifunc, *clear_w)
                                    << "(" << clear_this << ")";
    }
  }

  out << ");\n";
  indent(out, indent_level + 2) << "}\n";
  indent(out, indent_level) << "}\n\n";
}



/**
 *
 */
void InterfaceMakerCSharp::
write_dispose_pattern(ostream &out, Object *object, int indent_level) {
  string destructor_name = get_destructor_wrapper_name(object);
  string base_class = get_base_class_clause(object->_itype);

  if (destructor_name.empty() && base_class != "NativeObject") {
    return;
  }

  // Check if this type is RefCounted so we can dispatch to unref_delete.
  TypeIndex type_index = get_type_index_for_interrogate_type(object->_itype);
  bool is_refcounted = (type_index != 0 && is_type_refcounted(type_index));
  string unref_name = is_refcounted ? get_supplemental_unref_destructor_name(object->_itype) : "";

  indent(out, indent_level) << "protected override void ReleaseNative() {\n";
  if (!destructor_name.empty()) {
    if (is_refcounted) {
      indent(out, indent_level + 2) << "if (Ownership == NativeOwnership.RefCounted) {\n";
      indent(out, indent_level + 4) << "NativeMethods." << unref_name << "(NativeHandle);\n";
      indent(out, indent_level + 2) << "} else {\n";
      indent(out, indent_level + 4) << "NativeMethods." << destructor_name << "(NativeHandle);\n";
      indent(out, indent_level + 2) << "}\n";
    } else {
      indent(out, indent_level + 2) << "NativeMethods." << destructor_name << "(NativeHandle);\n";
    }
  }
  indent(out, indent_level) << "}\n";
}

/**
 *
 */
void InterfaceMakerCSharp::
write_globals_file(const string &dir, const string &cs_namespace,
                   InterrogateModuleDef *def) {
  std::ofstream out;
  string globals_name = get_globals_class_name(
    (def != nullptr && def->module_name != nullptr) ? def->module_name : module_name);

  if (!open_output_file(dir, globals_name + ".cs", out)) {
    return;
  }

  out << "using System;\n"
      << "using System.Collections.Generic;\n"
      << "using Interrogate;\n"
      << "using System.Runtime.InteropServices;\n";
  emit_peer_namespace_usings(out, cs_namespace, dir);
  out << "\n"
      << "namespace " << cs_namespace << " {\n"
      << "  public static class " << globals_name << " {\n";

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();

  int num_manifests = idb->get_num_global_manifests();
  std::set<string> method_signatures;
  std::set<string> emitted_manifests;
  for (int i = 0; i < num_manifests; ++i) {
    const InterrogateManifest &iman = idb->get_manifest(idb->get_global_manifest(i));
    string manifest_name = make_csharp_identifier(iman.get_name());
    if (!emitted_manifests.insert(manifest_name).second) {
      continue;
    }
    method_signatures.insert("N:" + manifest_name);

    if (iman.has_int_value()) {
      indent(out, 4) << "public const int " << manifest_name << " = "
                     << iman.get_int_value() << ";\n";
      continue;
    }

    if (iman.has_getter()) {
      FunctionsByIndex::const_iterator fi = _functions.find(iman.get_getter());
      if (fi != _functions.end()) {
        Function *func = (*fi).second;
        FunctionRemap *remap = first_legal_remap(func);
        if (remap != nullptr && !remap->_void_return) {
          bool return_nullable = is_return_nullable(remap);
          string return_type = get_csharp_type_for_wrapper(remap->_return_type, return_nullable);
          string native_call = "NativeMethods." + get_pinvoke_name(func, remap) + "()";
          string pinvoke_return_type = get_pinvoke_type(remap->_return_type->get_new_type(), true);

          indent(out, 4) << "public static " << return_type << " " << manifest_name << " {\n";
          indent(out, 6) << "get {\n";

          CPPType *return_type_cpp = remap->_return_type->get_new_type();
          if (TypeManager::is_bool(return_type_cpp) ||
              TypeManager::is_simple(return_type_cpp) ||
              TypeManager::is_enum(return_type_cpp)) {
            if (TypeManager::is_enum(return_type_cpp) && is_enum_type(_objects, return_type_cpp)) {
              indent(out, 8) << "return (" << return_type << ")" << native_call << ";\n";
            } else {
              indent(out, 8) << "return " << native_call << ";\n";
            }
          } else if (TypeManager::is_char_pointer(return_type_cpp) ||
                     TypeManager::is_const_char_pointer(return_type_cpp) ||
                     return_type == "string" || return_type == "string?") {
            if (pinvoke_return_type == "string") {
              if (return_nullable) {
                indent(out, 8) << "return " << native_call << ";\n";
              } else {
                indent(out, 8) << "return " << native_call
                               << " ?? throw new InvalidOperationException(\"Native getter returned null.\");\n";
              }
            } else {
              indent(out, 8) << "IntPtr result = " << native_call << ";\n";
              if (return_nullable) {
                indent(out, 8)
                  << "return result == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(result);\n";
              } else {
                indent(out, 8)
                  << "return result == IntPtr.Zero ? throw new InvalidOperationException(\"Native getter returned null.\") : Marshal.PtrToStringUTF8(result)!;\n";
              }
            }
          } else if (is_csharp_native_object_type(_objects, return_type_cpp, return_type)) {
            indent(out, 8) << "IntPtr result = " << native_call << ";\n";
            indent(out, 8) << "return " << return_type
                           << ".__CreateFromNative(result, "
                           << get_native_ownership_name(remap, false)
                           << ")";
            if (!return_nullable) {
              out << " ?? throw new InvalidOperationException(\"Native getter returned null.\")";
            }
            out << ";\n";
          } else {
            indent(out, 8) << "return " << native_call << ";\n";
          }

          indent(out, 6) << "}\n";
          indent(out, 4) << "}\n\n";
        }
      }
    }
  }

  auto reserve_method_signatures = [&](Function *method) {
    if (method == nullptr) {
      return;
    }

    string reserved_name = make_csharp_identifier(method->_ifunc.get_name());
    Function::Remaps::const_iterator ri;
    for (ri = method->_remaps.begin(); ri != method->_remaps.end(); ++ri) {
      FunctionRemap *remap = (*ri);
      if ((remap->_flags & FunctionRemap::F_explicit_self) != 0 ||
          remap->_type == FunctionRemap::T_constructor ||
          remap->_type == FunctionRemap::T_destructor ||
          !is_remap_legal_csharp(remap)) {
        continue;
      }

      bool is_static = !remap->_has_this;
      size_t first_param = remap->_has_this ? 1 : 0;
      std::vector<string> reserved_param_types;
      for (size_t i = first_param; i < remap->_parameters.size(); ++i) {
        reserved_param_types.push_back(
          get_csharp_signature_type_for_wrapper(remap->_parameters[i]._remap, false));
      }

      method_signatures.insert("R:" +
        build_signature_key(reserved_name, reserved_param_types, is_static));
    }
  };

  int num_functions = idb->get_num_global_functions();
  for (int i = 0; i < num_functions; ++i) {
    FunctionIndex func_index = idb->get_global_function(i);
    FunctionsByIndex::const_iterator fi = _functions.find(func_index);
    if (fi != _functions.end()) {
      reserve_method_signatures((*fi).second);
    }
  }

  for (int i = 0; i < num_functions; ++i) {
    FunctionIndex func_index = idb->get_global_function(i);
    FunctionsByIndex::const_iterator fi = _functions.find(func_index);
    if (fi != _functions.end()) {
      write_method(out, (*fi).second, nullptr, 4, false, &method_signatures);
    }
  }

  out << "  }\n"
      << "}\n";
}

/**
 *
 */
void InterfaceMakerCSharp::
write_dllimport(ostream &out, FunctionRemap *remap, const string &friendly_name) {
  const InterrogateFunctionWrapper *wrapper = nullptr;
  if (remap != nullptr && remap->_wrapper_index != 0) {
    wrapper = &InterrogateDatabase::get_ptr()->get_wrapper(remap->_wrapper_index);
  }

  bool has_string_param = false;
  bool has_string_return = false;
  if (wrapper != nullptr) {
    for (int i = 0; i < wrapper->number_of_parameters(); ++i) {
      if (get_pinvoke_type(wrapper->parameter_get_type(i), false) == "string") {
        has_string_param = true;
        break;
      }
    }
    has_string_return = get_pinvoke_type(wrapper->get_return_type(), true) == "string";
  } else {
    for (size_t i = 0; i < remap->_parameters.size(); ++i) {
      CPPType *t = remap->_parameters[i]._remap->get_new_type();
      if (TypeManager::is_char_pointer(t) || TypeManager::is_const_char_pointer(t)) {
        has_string_param = true;
        break;
      }
    }
    has_string_return = get_pinvoke_type(remap->_return_type->get_new_type(), true) == "string";
  }

  out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name)
      << "\", EntryPoint = \"" << get_c_wrapper_name(remap) << "\"";
  if (has_string_param || has_string_return) {
    out << ", StringMarshalling = StringMarshalling.Utf8";
  }
  out << ")]\n";

  string return_attr =
    (wrapper != nullptr)
      ? get_marshal_attribute(wrapper->get_return_type(), true)
      : get_marshal_attribute(remap->_return_type->get_new_type(), true);
  if (!return_attr.empty()) {
    out << "    " << return_attr << "\n";
  }

  bool return_nullable = (wrapper != nullptr) ? wrapper->is_return_nullable() : is_return_nullable(remap);
  out << "    internal static partial "
      << ((wrapper != nullptr)
            ? get_pinvoke_type(wrapper->get_return_type(), true)
            : get_pinvoke_type(remap->_return_type->get_new_type(), true))
      << ((((wrapper != nullptr)
             ? get_pinvoke_type(wrapper->get_return_type(), true)
             : get_pinvoke_type(remap->_return_type->get_new_type(), true)) == "string" && return_nullable) ? "?" : "")
      << " " << friendly_name << "(";

  int num_parameters = (wrapper != nullptr) ? wrapper->number_of_parameters() : (int)remap->_parameters.size();
  for (int i = 0; i < num_parameters; ++i) {
    if (i != 0) {
      out << ", ";
    }

    string attr;
    string pinvoke_type;
    if (wrapper != nullptr) {
      TypeIndex type = wrapper->parameter_get_type(i);
      attr = get_marshal_attribute(type, false);
      pinvoke_type = get_pinvoke_type(type, false);
      if (pinvoke_type == "string" && wrapper->parameter_is_nullable(i)) {
        pinvoke_type += "?";
      }
    } else {
      CPPType *type = remap->_parameters[i]._remap->get_new_type();
      attr = get_marshal_attribute(type, false);
      pinvoke_type = get_pinvoke_type(type, false);
      if (pinvoke_type == "string" && is_parameter_nullable(remap, i)) {
        pinvoke_type += "?";
      }
    }
    if (!attr.empty()) {
      out << attr << " ";
    }

    string param_name = get_csharp_parameter_name(remap, i);
    out << pinvoke_type << " " << param_name;
  }

  out << ");\n";
}

/**
 *
 */
void InterfaceMakerCSharp::
write_dllimport(ostream &out, const InterrogateFunction &ifunc,
                const InterrogateFunctionWrapper &wrapper,
                const string &friendly_name) {
  bool has_string_param = false;
  for (int i = 0; i < wrapper.number_of_parameters(); ++i) {
    if (get_pinvoke_type(wrapper.parameter_get_type(i), false) == "string") {
      has_string_param = true;
      break;
    }
  }
  bool has_string_return = get_pinvoke_type(wrapper.get_return_type(), true) == "string";

  out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name)
      << "\", EntryPoint = \"" << wrapper.get_name() << "\"";
  if (has_string_param || has_string_return) {
    out << ", StringMarshalling = StringMarshalling.Utf8";
  }
  out << ")]\n";

  string return_attr = get_marshal_attribute(wrapper.get_return_type(), true);
  if (!return_attr.empty()) {
    out << "    " << return_attr << "\n";
  }

  bool return_nullable = wrapper.is_return_nullable();
  out << "    internal static partial "
      << get_pinvoke_type(wrapper.get_return_type(), true)
      << ((get_pinvoke_type(wrapper.get_return_type(), true) == "string" && return_nullable) ? "?" : "")
      << " " << friendly_name << "(";

  std::set<string> used_param_names;
  for (int i = 0; i < wrapper.number_of_parameters(); ++i) {
    if (i != 0) {
      out << ", ";
    }

    TypeIndex type = wrapper.parameter_get_type(i);
    string attr = get_marshal_attribute(type, false);
    if (!attr.empty()) {
      out << attr << " ";
    }

    string param_name = wrapper.parameter_has_name(i)
      ? make_csharp_identifier(wrapper.parameter_get_name(i))
      : string("param") + std::to_string(i);

    // Deduplicate: if the name is already used, append the index.
    if (!used_param_names.insert(param_name).second) {
      param_name += std::to_string(i);
      used_param_names.insert(param_name);
    }

    string pinvoke_type = wrapper.parameter_is_this(i) ? "IntPtr" : get_pinvoke_type(type, false);
    if (pinvoke_type == "string" && wrapper.parameter_is_nullable(i)) {
      pinvoke_type += "?";
    }
    if (!wrapper.parameter_is_this(i) && wrapper.parameter_is_out(i)) {
      string out_type = csharp_out_parameter_type(type);
      if (!out_type.empty()) {
        pinvoke_type = "out " + out_type;
      }
    }
    out << pinvoke_type << " " << param_name;
  }

  out << ");\n";
}

/**
 *
 */
string InterfaceMakerCSharp::
get_c_wrapper_name(FunctionRemap *remap) const {
  return string("_inC") + _def->library_hash_name + remap->_hash;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_pinvoke_name(Function *func, FunctionRemap *remap) const {
  string base_name;
  if (func != nullptr && func->_ifunc.has_scoped_name()) {
    base_name = func->_ifunc.get_scoped_name();
  } else if (func != nullptr && func->_ifunc.has_name()) {
    base_name = func->_ifunc.get_name();
  } else if (func != nullptr) {
    base_name = func->_name;
  } else {
    base_name = "Wrapper";
  }

  return make_csharp_identifier(base_name + "_" + remap->_hash);
}

/**
 *
 */
string InterfaceMakerCSharp::
get_pinvoke_name(const InterrogateFunction &ifunc,
                 const InterrogateFunctionWrapper &wrapper) const {
  string base_name;
  if (ifunc.has_scoped_name()) {
    base_name = ifunc.get_scoped_name();
  } else if (ifunc.has_name()) {
    base_name = ifunc.get_name();
  } else {
    base_name = "Wrapper";
  }

  return make_csharp_identifier(base_name + "_" + wrapper.get_unique_name());
}

/**
 *
 */
bool InterfaceMakerCSharp::
is_current_native_methods_type(const InterrogateType &itype) const {
  // In database-only pass (pass 2), only this module's .in files are loaded
  // as command-line args.  However, base-class stubs from foreign modules can
  // still end up in the InterrogateDatabase (and therefore in _objects) because
  // the .in format encodes cross-module type references.  Use
  // csharp_owned_type_indices to filter: only emit types whose TypeIndex was
  // recorded as belonging to this module during main().
  if (csharp_database_only_pass) {
    if (!csharp_owned_type_indices.empty()) {
      // Collection facade types (vector_uchar, vector_string, etc.) are C++
      // typedefs, not PUBLISHED classes, so they carry neither F_global nor
      // F_nested and are excluded from csharp_owned_type_indices.  Gate their
      // generation on the module they are attributed to in csharp_type_module_map
      // so that each collection type is generated by exactly one module.
      if (is_collection_facade_type(itype)) {
        string facade_module = get_type_module_name(itype);
        return facade_module.empty() || facade_module == _current_module_name;
      }
      TypeIndex tidx = get_type_index_for_interrogate_type(itype);
      return tidx != 0 && csharp_owned_type_indices.count(tidx) != 0;
    }
    return true;
  }

  if (itype.has_library_name()) {
    string lib = itype.get_library_name();
    if (!lib.empty()) {
      return lib == _current_library_name;
    }
  }

  if (!_current_module_name.empty() && itype.has_module_name() &&
      itype.get_module_name() != _current_module_name) {
    return false;
  }

  return true;
}

/**
 * Overload that uses a pre-known TypeIndex (e.g. oi->first from _objects) to
 * avoid the scoped-name lookup in is_current_native_methods_type(itype), which
 * can return the wrong index after load_all_search_dir_databases() loads
 * forward-reference duplicates at higher indices.
 */
bool InterfaceMakerCSharp::
is_current_native_methods_type(TypeIndex tidx, const InterrogateType &itype) const {
  if (csharp_database_only_pass && !csharp_owned_type_indices.empty()) {
    if (is_collection_facade_type(itype)) {
      string facade_module = get_type_module_name(itype);
      return facade_module.empty() || facade_module == _current_module_name;
    }
    return tidx != 0 && csharp_owned_type_indices.count(tidx) != 0;
  }
  return is_current_native_methods_type(itype);
}

/**
 *
 */
string InterfaceMakerCSharp::
get_native_methods_class_name(const InterrogateType &itype) const {
  if (csharp_database_only_pass) {
    return "NativeMethods";
  }

  const char *module_name = itype.has_module_name() ? itype.get_module_name() : nullptr;
  if (module_name == nullptr || module_name[0] == '\0') {
    return "NativeMethods";
  }

  if (_current_module_name.empty() || string(module_name) == _current_module_name) {
    return "NativeMethods";
  }

  return "global::" + prettify_namespace(module_name) + ".NativeMethods";
}

/**
 *
 */
string InterfaceMakerCSharp::
get_pinvoke_call_name(Function *func, FunctionRemap *remap) const {
  if (func == nullptr) {
    return "NativeMethods." + get_pinvoke_name(func, remap);
  }

  return get_native_methods_class_name(func->_itype) + "." +
    get_pinvoke_name(func, remap);
}

/**
 *
 */
string InterfaceMakerCSharp::
get_pinvoke_call_name(const InterrogateFunction &ifunc,
                      const InterrogateFunctionWrapper &wrapper) const {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  if (!ifunc.is_method() || ifunc.get_class() == 0) {
    return "NativeMethods." + get_pinvoke_name(ifunc, wrapper);
  }

  const InterrogateType &itype = idb->get_type(ifunc.get_class());
  return get_native_methods_class_name(itype) + "." + get_pinvoke_name(ifunc, wrapper);
}

/**
 *
 */
string InterfaceMakerCSharp::
get_native_this_argument(Object *object, Function *func, FunctionRemap *remap) const {
  if (object == nullptr || func == nullptr || remap == nullptr || !remap->_has_this) {
    return "NativeHandle";
  }

  TypeIndex declaring_type = 0;
  if (func->_ifunc.is_method()) {
    declaring_type = func->_ifunc.get_class();
  }
  if (declaring_type == 0) {
    declaring_type = get_type_index_for_interrogate_type(func->_itype);
  }
  if (declaring_type == 0) {
    declaring_type = get_type_index_for_cpp_type(remap->_cpptype, remap->_cppscope);
  }
  TypeIndex object_type = get_type_index_for_interrogate_type(object->_itype);
  if (declaring_type == 0 || object_type == 0 || declaring_type == object_type) {
    return "NativeHandle";
  }

  std::vector<string> pinvoke_names;
  std::set<TypeIndex> visited;
  if (!find_upcast_chain(object->_itype, declaring_type, pinvoke_names, visited)) {
    return "NativeHandle";
  }

  string native_this = "NativeHandle";
  for (std::vector<string>::const_iterator ni = pinvoke_names.begin();
       ni != pinvoke_names.end(); ++ni) {
    native_this = (*ni) + "(" + native_this + ")";
  }

  return native_this;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_native_this_argument(const InterrogateType &object_type,
                         const InterrogateFunction &ifunc) const {
  if (!ifunc.is_method()) {
    return "NativeHandle";
  }

  TypeIndex declaring_type = ifunc.get_class();
  TypeIndex object_type_index = get_type_index_for_interrogate_type(object_type);
  if (declaring_type == 0 || object_type_index == 0 || declaring_type == object_type_index) {
    return "NativeHandle";
  }

  std::vector<string> pinvoke_names;
  std::set<TypeIndex> visited;
  if (!find_upcast_chain(object_type, declaring_type, pinvoke_names, visited)) {
    return "NativeHandle";
  }

  string native_this = "NativeHandle";
  for (std::vector<string>::const_iterator ni = pinvoke_names.begin();
       ni != pinvoke_names.end(); ++ni) {
    native_this = (*ni) + "(" + native_this + ")";
  }

  return native_this;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_upcast_pinvoke_name(const InterrogateType &itype, int derivation_index) const {
  if (!itype.derivation_has_upcast(derivation_index)) {
    return string();
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  FunctionIndex func_index = itype.derivation_get_upcast(derivation_index);
  FunctionsByIndex::const_iterator fi = _functions.find(func_index);
  if (fi == _functions.end() || (*fi).second == nullptr) {
    const InterrogateFunction &upcast_ifunc = idb->get_function(func_index);
    string target_scoped = upcast_ifunc.has_scoped_name() ? upcast_ifunc.get_scoped_name() : string();
    string target_name = upcast_ifunc.has_name() ? upcast_ifunc.get_name() : string();
    TypeIndex target_class = upcast_ifunc.is_method() ? upcast_ifunc.get_class() : 0;

    for (FunctionsByIndex::const_iterator it = _functions.begin(); it != _functions.end(); ++it) {
      Function *candidate = (*it).second;
      if (candidate == nullptr) {
        continue;
      }
      if (target_class != 0 && (!candidate->_ifunc.is_method() || candidate->_ifunc.get_class() != target_class)) {
        continue;
      }

      bool name_match = false;
      if (!target_scoped.empty() && candidate->_ifunc.has_scoped_name() &&
          candidate->_ifunc.get_scoped_name() == target_scoped) {
        name_match = true;
      } else if (!target_name.empty() && candidate->_ifunc.has_name() &&
                 candidate->_ifunc.get_name() == target_name) {
        name_match = true;
      }
      if (!name_match) {
        continue;
      }

      Function::Remaps::const_iterator cri;
      for (cri = candidate->_remaps.begin(); cri != candidate->_remaps.end(); ++cri) {
        FunctionRemap *remap = (*cri);
        if ((remap->_flags & FunctionRemap::F_explicit_self) != 0 ||
            !is_remap_legal_csharp(remap)) {
          continue;
        }
        return get_pinvoke_call_name(candidate, remap);
      }

      int num_candidate_wrappers = candidate->_ifunc.number_of_c_wrappers();
      for (int wi = 0; wi < num_candidate_wrappers; ++wi) {
        FunctionWrapperIndex wrapper_index = candidate->_ifunc.get_c_wrapper(wi);
        if (wrapper_index == 0) {
          continue;
        }

        const InterrogateFunctionWrapper &wrapper = idb->get_wrapper(wrapper_index);
        if (!is_wrapper_legal_csharp(wrapper) || wrapper.is_explicit_self()) {
          continue;
        }
        return get_pinvoke_call_name(candidate->_ifunc, wrapper);
      }
    }

    return string();
  }

  Function *func = (*fi).second;
  Function::Remaps::const_iterator ri;
  for (ri = func->_remaps.begin(); ri != func->_remaps.end(); ++ri) {
    FunctionRemap *remap = (*ri);
    if ((remap->_flags & FunctionRemap::F_explicit_self) != 0 ||
        !is_remap_legal_csharp(remap)) {
      continue;
    }

    return get_pinvoke_call_name(func, remap);
  }

  int num_wrappers = func->_ifunc.number_of_c_wrappers();
  for (int wi = 0; wi < num_wrappers; ++wi) {
    FunctionWrapperIndex wrapper_index = func->_ifunc.get_c_wrapper(wi);
    if (wrapper_index == 0) {
      continue;
    }

    const InterrogateFunctionWrapper &wrapper = InterrogateDatabase::get_ptr()->get_wrapper(wrapper_index);
    if (!is_wrapper_legal_csharp(wrapper) || wrapper.is_explicit_self()) {
      continue;
    }

    return get_pinvoke_call_name(func->_ifunc, wrapper);
  }

  return string();
}

/**
 *
 */
void InterfaceMakerCSharp::
get_secondary_base_types(const InterrogateType &itype,
                         std::vector<const InterrogateType *> &bases) const {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  auto has_members = [](const InterrogateType &candidate) {
    return candidate.number_of_methods() != 0 ||
           candidate.number_of_casts() != 0 ||
           candidate.number_of_elements() != 0;
  };

  std::set<TypeIndex> root_primary_chain;
  TypeIndex root_primary = 0;
  if (itype.number_of_derivations() != 0) {
    TypeIndex base_index = itype.get_derivation(0);
    if (is_wrapped_type(base_index)) {
      root_primary = base_index;
    }
  }
  while (root_primary != 0 && root_primary_chain.insert(root_primary).second) {
    const InterrogateType &base_type = idb->get_type(root_primary);
    if (base_type.number_of_derivations() == 0) {
      break;
    }
    TypeIndex next_primary = base_type.get_derivation(0);
    root_primary = is_wrapped_type(next_primary) ? next_primary : 0;
  }

  std::set<TypeIndex> seen_bases;
  std::function<void(const InterrogateType &, bool)> visit =
    [&](const InterrogateType &current, bool include_primary_chain) {
      TypeIndex current_primary = 0;
      if (current.number_of_derivations() != 0) {
        TypeIndex base_index = current.get_derivation(0);
        if (is_wrapped_type(base_index)) {
          current_primary = base_index;
        }
      }

      int num_derivations = current.number_of_derivations();
      for (int di = 0; di < num_derivations; ++di) {
        TypeIndex base_index = current.get_derivation(di);
        if (base_index == 0 || !is_wrapped_type(base_index)) {
          continue;
        }
        if (!include_primary_chain && base_index == current_primary) {
          continue;
        }
        if (root_primary_chain.find(base_index) != root_primary_chain.end() ||
            !seen_bases.insert(base_index).second) {
          continue;
        }

        const InterrogateType *base_type = &idb->get_type(base_index);
        if (should_skip_csharp_type(*base_type)) {
          continue;
        }
        if (!uses_csharp_interface(*base_type)) {
          continue;
        }

        if (!has_members(*base_type) && ensure_database_loaded(*base_type)) {
          base_type = &idb->get_type(base_index);
        }

        if (!has_members(*base_type)) {
          string scoped_name;
          if (base_type->has_scoped_name()) {
            scoped_name = base_type->get_scoped_name();
          } else if (base_type->_cpptype != nullptr) {
            CPPType *resolved = TypeManager::resolve_type(base_type->_cpptype);
            if (resolved != nullptr) {
              scoped_name = resolved->get_fully_scoped_name();
            }
          }

          if (!scoped_name.empty()) {
            TypeIndex resolved_index = idb->lookup_type_by_scoped_name(scoped_name);
            if (resolved_index != 0) {
              const InterrogateType &resolved_type = idb->get_type(resolved_index);
              if (has_members(resolved_type)) {
                base_type = &resolved_type;
              }
            }
          }
        }

        if (!has_members(*base_type)) {
          continue;
        }

        bases.push_back(base_type);
        visit(*base_type, true);
      }
    };

  visit(itype, false);
}

/**
 *
 */
bool InterfaceMakerCSharp::
ensure_database_loaded(const InterrogateType &itype) const {
  if (itype.has_library_name()) {
    string library = itype.get_library_name();
    if (!library.empty() && library != _current_library_name) {
      Filename database_file = find_database_file(library + ".in");
      if (!database_file.empty()) {
        request_external_database(database_file);
        return true;
      }

      return false;
    }
  }

  return false;
}

/**
 *
 */
void InterfaceMakerCSharp::
request_external_database(const Filename &database_file) const {
  string database_basename = database_file.get_basename();
  if (!_loaded_external_databases.insert(database_basename).second) {
    return;
  }

  _external_database_paths.push_back(database_file.to_os_specific());

  InterrogateModuleDef def = {};
  def.database_filename = _external_database_paths.back().c_str();
  _external_database_requests.push_back(def);

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  idb->request_module(&_external_database_requests.back());
  (void)idb->get_num_all_types();
  _csharp_interface_cache_valid = false;
}



/**
 *
 */
bool InterfaceMakerCSharp::
find_upcast_chain(const InterrogateType &itype, TypeIndex target_type,
                  std::vector<string> &pinvoke_names,
                  std::set<TypeIndex> &visited) const {
  TypeIndex resolved_type = get_type_index_for_interrogate_type(itype);
  if (resolved_type == 0) {
    return false;
  }
  if (resolved_type == target_type) {
    return true;
  }
  if (!visited.insert(resolved_type).second) {
    return false;
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  int num_derivations = itype.number_of_derivations();
  for (int di = 0; di < num_derivations; ++di) {
    TypeIndex base_index = itype.get_derivation(di);
    if (base_index == 0) {
      continue;
    }

    const InterrogateType &base_type = idb->get_type(base_index);

    // For secondary bases (di > 0), we need the upcast function.
    // For the primary base (di == 0), no upcast is needed (same address), so we skip adding it.
    string upcast_name;
    if (di > 0) {
      upcast_name = get_upcast_pinvoke_name(itype, di);
      if (upcast_name.empty()) {
        continue;
      }
      pinvoke_names.push_back(upcast_name);
    }

    if (find_upcast_chain(base_type, target_type, pinvoke_names, visited)) {
      return true;
    }

    if (di > 0) {
      pinvoke_names.pop_back();
    }
  }

  return false;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_pinvoke_type(CPPType *type, bool for_return) const {
  if (type == nullptr) {
    return "IntPtr";
  }

  type = TypeManager::resolve_type(type);

  if (TypeManager::is_void(type)) {
    return "void";
  }
  if (TypeManager::is_bool(type)) {
    return "bool";
  }
  if (TypeManager::is_char_pointer(type) ||
      TypeManager::is_const_char_pointer(type)) {
    return for_return ? "IntPtr" : "string";
  }
  if (TypeManager::is_pointer(type)) {
    return "IntPtr";
  }
  if (TypeManager::is_enum(type)) {
    return "int";
  }
  if (TypeManager::is_struct(type)) {
    return "IntPtr";
  }
  if (TypeManager::is_simple(type)) {
    CPPType *unwrapped = TypeManager::unwrap(type);
    string type_name = unwrapped->get_local_name(&parser);
    if (type_name == "int") return "int";
    if (type_name == "unsigned int") return "uint";
    if (type_name == "float") return "float";
    if (type_name == "double") return "double";
    if (type_name == "long long") return "long";
    if (type_name == "unsigned long long") return "ulong";
    if (type_name == "short") return "short";
    if (type_name == "unsigned short") return "ushort";
    if (type_name == "long") return "int";
    if (type_name == "unsigned long") return "uint";
    if (type_name == "char") return "byte";
    if (type_name == "unsigned char") return "byte";
    if (type_name == "signed char") return "sbyte";

    if (TypeManager::is_integer(unwrapped)) return "int";
    if (TypeManager::is_float(unwrapped)) return "float";
    return "int";
  }

  return "IntPtr";
}

/**
 *
 */
string InterfaceMakerCSharp::
get_pinvoke_type(TypeIndex type_index, bool for_return) const {
  type_index = unwrap_type_aliases(type_index);
  if (type_index == 0) {
    return "IntPtr";
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateType &itype = idb->get_type(type_index);

  if (itype.is_atomic()) {
    return get_pinvoke_atomic_type(itype, for_return);
  }
  if (itype.is_pointer()) {
    TypeIndex inner = unwrap_type_aliases(itype.get_wrapped_type());
    if (inner != 0 && is_char_type(inner)) {
      return for_return ? "IntPtr" : "string";
    }
    return "IntPtr";
  }
  if (itype.is_enum()) {
    return "int";
  }
  if (itype.is_class() || itype.is_struct() || itype.is_union()) {
    return "IntPtr";
  }
  if (itype.is_wrapped()) {
    return get_pinvoke_type(itype.get_wrapped_type(), for_return);
  }

  return "IntPtr";
}

/**
 *
 */
string InterfaceMakerCSharp::
get_csharp_type(CPPType *type, bool for_return) const {
  if (type == nullptr) {
    return "IntPtr";
  }

  type = TypeManager::resolve_type(type);
  CPPType *lookup_type = TypeManager::unwrap_const(TypeManager::unwrap(type));
  if (lookup_type != nullptr) {
    lookup_type = TypeManager::resolve_type(lookup_type);
  } else {
    lookup_type = type;
  }

  if (TypeManager::is_void(type)) {
    return "void";
  }
  if (TypeManager::is_bool(type)) {
    return "bool";
  }
  if (TypeManager::is_char_pointer(type) ||
      TypeManager::is_const_char_pointer(type)) {
    return for_return ? "string?" : "string";
  }

  const InterrogateType *itype = find_interrogate_type(_objects, lookup_type);
  if (itype != nullptr) {
    if (itype->is_wrapped()) {
      return get_csharp_type(itype->get_wrapped_type(), for_return);
    }
    if (is_collection_facade_type(*itype)) {
      return get_qualified_class_name(*itype) + (for_return ? "?" : "");
    }
    if (itype->is_enum()) {
      return get_qualified_class_name(*itype);
    }
    if (itype->is_class() || itype->is_struct()) {
      return get_qualified_class_name(*itype) + (for_return ? "?" : "");
    }
  }

  if (TypeManager::is_pointer(type) || TypeManager::is_reference(type)) {
    CPPType *inner = TypeManager::unwrap(type);
    if (inner != nullptr) {
      const InterrogateType *inner_itype = find_interrogate_type(_objects, inner);
      if (inner_itype != nullptr && (inner_itype->is_class() || inner_itype->is_struct())) {
          return get_qualified_class_name(*inner_itype) + (for_return ? "?" : "");
      }
    }
  }

  if (TypeManager::is_enum(type)) {
    return "int";
  }

  return get_pinvoke_type(type, for_return);
}

/**
 *
 */
string InterfaceMakerCSharp::
get_csharp_type(TypeIndex type_index, bool for_return) const {
  TypeIndex original_type_index = type_index;
  type_index = unwrap_type_aliases(type_index);
  if (type_index == 0 && original_type_index == 0) {
    return "IntPtr";
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  if (original_type_index != 0) {
    const InterrogateType &original_type = idb->get_type(original_type_index);
    if (is_collection_facade_type(original_type)) {
      return get_qualified_class_name(original_type) + (for_return ? "?" : "");
    }
    if (is_empty_pointer_facade_type(original_type)) {
      TypeIndex inner_index = get_type_index_for_cpp_type(get_pointer_facade_pointee_cpp_type(original_type));
      if (inner_index != 0 && inner_index != original_type_index) {
        return get_csharp_type(inner_index, for_return);
      }
      return get_pointer_facade_target_name(original_type) + (for_return ? "?" : "");
    }
  }

  if (type_index == 0) {
    return "IntPtr";
  }

  const InterrogateType &itype = idb->get_type(type_index);

  if (is_collection_facade_type(itype)) {
    return get_qualified_class_name(itype) + (for_return ? "?" : "");
  }

  if (is_empty_pointer_facade_type(itype)) {
    TypeIndex inner_index = get_type_index_for_cpp_type(get_pointer_facade_pointee_cpp_type(itype));
    if (inner_index != 0 && inner_index != type_index) {
      return get_csharp_type(inner_index, for_return);
    }
    return get_pointer_facade_target_name(itype) + (for_return ? "?" : "");
  }

  if (itype.is_atomic()) {
    AtomicToken tok = itype.get_atomic_token();
    if (tok == AT_string) {
      return for_return ? "string?" : "string";
    }
    if (tok == AT_istream || tok == AT_ostream || tok == AT_iostream) {
      // Default mapping for atomic stream tokens is IntPtr — that's the
      // correct managed type for return values (since we don't yet support
      // reverse bridging: wrapping a C++ stream as a System.IO.Stream).
      //
      // Parameters are overridden to System.IO.Stream in write_method /
      // write_constructor by checking csharp_stream_token_for_type before
      // calling this function; those callsites emit a StreamBridge.For…
      // using block around the P/Invoke call.
      return "IntPtr";
    }
    return get_pinvoke_atomic_type(itype, for_return);
  }
  if (itype.is_pointer()) {
    TypeIndex inner = unwrap_type_aliases(itype.get_wrapped_type());
    if (inner != 0 && is_char_type(inner)) {
      return for_return ? "string?" : "string";
    }
    if (inner != 0) {
      const InterrogateType &inner_type = idb->get_type(inner);
      if (inner_type.is_class() || inner_type.is_struct()) {
        if (is_collection_facade_type(inner_type)) {
          return get_qualified_class_name(inner_type) + (for_return ? "?" : "");
        }
        return get_public_native_object_type_name(inner_type, for_return);
      }
    }
    return "IntPtr";
  }
  if (itype.is_enum()) {
    return get_qualified_class_name(itype);
  }
  if (itype.is_class() || itype.is_struct()) {
    return get_public_native_object_type_name(itype, for_return);
  }
  if (itype.is_wrapped()) {
    return get_csharp_type(itype.get_wrapped_type(), for_return);
  }

  return get_pinvoke_type(type_index, for_return);
}

/**
 *
 */
string InterfaceMakerCSharp::
get_csharp_type_for_wrapper(ParameterRemap *remap, bool for_return) const {
  if (remap == nullptr) {
    return "IntPtr";
  }

  TypeIndex type_index = get_type_index_for_cpp_type(remap->get_new_type());
  if (type_index != 0) {
    return get_csharp_type(type_index, for_return);
  }

  return get_csharp_type(remap->get_new_type(), for_return);
}

/**
 *
 */
const InterrogateType *InterfaceMakerCSharp::
find_csharp_object_type(const string &type_name) const {
  if (type_name.empty()) {
    return nullptr;
  }

  Objects::const_iterator oi;
  for (oi = _objects.begin(); oi != _objects.end(); ++oi) {
    Object *object = (*oi).second;
    if (object == nullptr) {
      continue;
    }

    const InterrogateType &itype = object->_itype;
    if ((itype.is_class() || itype.is_struct()) && get_class_name(itype) == type_name) {
      return &itype;
    }
  }

  return nullptr;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_csharp_signature_type(CPPType *type, bool for_return) const {
  CPPType *lookup_type = TypeManager::resolve_type(type);
  lookup_type = TypeManager::unwrap_const(TypeManager::unwrap(lookup_type));
  if (lookup_type != nullptr) {
    lookup_type = TypeManager::resolve_type(lookup_type);
  } else {
    lookup_type = TypeManager::resolve_type(type);
  }

  const InterrogateType *itype = find_interrogate_type(_objects, lookup_type);
  if (itype != nullptr) {
    if (itype->is_wrapped()) {
      return get_csharp_signature_type(itype->get_wrapped_type(), for_return);
    }
    if (is_collection_facade_type(*itype)) {
      if (should_skip_csharp_type(*itype)) {
        return "";
      }
      return get_qualified_class_name(*itype) + (for_return ? "?" : "");
    }
    if (itype->is_class() || itype->is_struct()) {
      if (should_skip_csharp_type(*itype)) {
        return "";
      }
      return get_public_native_object_type_name(*itype, for_return);
    }
  }

  // Fallback: look up in global database for cross-module types
  CPPType *resolved_type = TypeManager::resolve_type(type);
  if (resolved_type != nullptr) {
    TypeIndex type_index = get_type_index_for_cpp_type(resolved_type);
    if (type_index != 0) {
      return get_csharp_signature_type(type_index, for_return);
    }
  }

  string type_name = get_csharp_type(type, for_return);
  const InterrogateType *named_type = find_csharp_object_type(type_name);
  if (named_type != nullptr) {
    if (should_skip_csharp_type(*named_type)) {
      return "";
    }
    return get_public_native_object_type_name(*named_type, for_return);
  }

  return type_name;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_csharp_signature_type(TypeIndex type_index, bool for_return,
                          bool is_parameter) const {
  TypeIndex original_type_index = type_index;
  type_index = unwrap_type_aliases(type_index);
  if (type_index == 0 && original_type_index == 0) {
    return "IntPtr";
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  if (original_type_index != 0) {
    const InterrogateType &original_type = idb->get_type(original_type_index);
    if (is_collection_facade_type(original_type)) {
      if (should_skip_csharp_type(original_type)) {
        return "";
      }
      // A const facade is the right type to RETURN -- the pointer really does
      // point into const storage -- but the wrong one to ACCEPT.  A `const T &`
      // parameter is remapped to `T const *`, and handing C++ a mutable T where
      // it wants a const T is always legal; demanding the read-only facade here
      // instead makes the method uncallable, because that class has no way to be
      // built.  Every other const-ref parameter already drops its const:
      // NodePath::set_pos takes LVecBase3f, not LVecBase3f_const.  Facades were
      // the sole exception, only because this check runs before the unwrap below.
      string facade_name = get_qualified_interface_name(original_type);

      if (is_parameter && is_const_qualified_type(original_type) &&
          facade_name.size() > 6 &&
          facade_name.compare(facade_name.size() - 6, 6, "_const") == 0) {
        // Drop the const by dropping the suffix, rather than by resolving to the
        // non-const TypeIndex.  Both class names come from the same C++ name --
        // "vector_uchar const" gives vector_uchar_const, "vector_uchar" gives
        // vector_uchar -- so removing "_const" always lands on the class that was
        // in fact generated.  Walking the type graph does not: a nested facade
        // like PolylightEffect::LightGroup has several database records, and the
        // non-const one there names a class that is never written.
        return facade_name.substr(0, facade_name.size() - 6);
      }
      return facade_name + (for_return ? "?" : "");
    }
    if (is_empty_pointer_facade_type(original_type)) {
      TypeIndex inner_index = get_type_index_for_cpp_type(get_pointer_facade_pointee_cpp_type(original_type));
      if (inner_index != 0 && inner_index != original_type_index) {
        return get_csharp_signature_type(inner_index, for_return);
      }
      return get_pointer_facade_target_name(original_type) + (for_return ? "?" : "");
    }
  }

  if (type_index == 0) {
    return "IntPtr";
  }

  const InterrogateType &itype = idb->get_type(type_index);

  if (is_collection_facade_type(itype)) {
    if (should_skip_csharp_type(itype)) {
      return "";
    }
    return get_qualified_interface_name(itype) + (for_return ? "?" : "");
  }

  if (is_empty_pointer_facade_type(itype)) {
    TypeIndex inner_index = get_type_index_for_cpp_type(get_pointer_facade_pointee_cpp_type(itype));
    if (inner_index != 0 && inner_index != type_index) {
      return get_csharp_signature_type(inner_index, for_return);
    }
    return get_pointer_facade_target_name(itype) + (for_return ? "?" : "");
  }

  if (itype.is_class() || itype.is_struct()) {
    // If this type is not exported (skipped), return "" so callers can skip
    // any method that references it rather than emitting an unresolvable IFoo.
    if (should_skip_csharp_type(itype)) {
      return "";
    }
    return get_public_native_object_type_name(itype, for_return);
  }
  if (itype.is_pointer()) {
    TypeIndex inner = unwrap_type_aliases(itype.get_wrapped_type());
    if (inner != 0) {
      const InterrogateType &inner_type = idb->get_type(inner);
      if (inner_type.is_class() || inner_type.is_struct()) {
        if (should_skip_csharp_type(inner_type)) {
          return "";
        }
        return get_public_native_object_type_name(inner_type, for_return);
      }
    }
  }

  return get_csharp_type(type_index, for_return);
}

/**
 *
 */
string InterfaceMakerCSharp::
get_csharp_signature_type_for_wrapper(ParameterRemap *remap, bool for_return) const {
  if (remap == nullptr) {
    return "IntPtr";
  }

  TypeIndex type_index = get_type_index_for_cpp_type(remap->get_new_type());
  if (type_index != 0) {
    return get_csharp_signature_type(type_index, for_return);
  }

  return get_csharp_signature_type(remap->get_new_type(), for_return);
}

/**
 *
 */
string InterfaceMakerCSharp::
get_csharp_native_object_class_name(CPPType *type) const {
  const InterrogateType *itype = find_interrogate_type(_objects, type);
  if (itype != nullptr && (itype->is_class() || itype->is_struct())) {
    return get_qualified_class_name(*itype);
  }

  // Fallback: look up in global database for cross-module types
  CPPType *resolved_type = TypeManager::resolve_type(type);
  if (resolved_type != nullptr) {
    TypeIndex type_index = get_type_index_for_cpp_type(resolved_type);
    if (type_index != 0) {
      return get_csharp_native_object_class_name(type_index);
    }
  }

  string type_name = get_csharp_type(type, false);
  const InterrogateType *named_type = find_csharp_object_type(type_name);
  if (named_type != nullptr) {
    return get_qualified_class_name(*named_type);
  }

  return string();
}

/**
 *
 */
string InterfaceMakerCSharp::
get_csharp_native_object_class_name(TypeIndex type_index) const {
  TypeIndex original_type_index = type_index;
  type_index = unwrap_type_aliases(type_index);
  if (type_index == 0 && original_type_index == 0) {
    return string();
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  if (original_type_index != 0) {
    const InterrogateType &original_type = idb->get_type(original_type_index);
    if (is_collection_facade_type(original_type)) {
      return get_qualified_class_name(original_type);
    }
    if (is_empty_pointer_facade_type(original_type)) {
      TypeIndex inner_index = get_type_index_for_cpp_type(get_pointer_facade_pointee_cpp_type(original_type));
      if (inner_index != 0 && inner_index != original_type_index) {
        return get_csharp_native_object_class_name(inner_index);
      }
      return get_pointer_facade_target_name(original_type);
    }
  }

  if (type_index == 0) {
    return string();
  }

  const InterrogateType &itype = idb->get_type(type_index);
  if (is_empty_pointer_facade_type(itype)) {
    TypeIndex inner_index = get_type_index_for_cpp_type(get_pointer_facade_pointee_cpp_type(itype));
    if (inner_index != 0 && inner_index != type_index) {
      return get_csharp_native_object_class_name(inner_index);
    }
    return get_pointer_facade_target_name(itype);
  }
  if (itype.is_class() || itype.is_struct()) {
    return globalize_class_name(itype);
  }
  if (itype.is_pointer()) {
    TypeIndex inner = unwrap_type_aliases(itype.get_wrapped_type());
    if (inner != 0) {
      const InterrogateType &inner_type = idb->get_type(inner);
      if (inner_type.is_class() || inner_type.is_struct()) {
        return globalize_class_name(inner_type);
      }
    }
  }

  return string();
}

/**
 * The class name, always fully qualified.  This function's result is only ever
 * used as the receiver of `.__CreateFromNative(...)`, and a bare name there can
 * be shadowed by a member of the same name -- C#'s "Color Color" rule.  Once
 * DisplayRegion gained a CullTraverser property, `CullTraverser.__CreateFromNative`
 * inside its own methods started resolving to the property (an ICullTraverser)
 * instead of the class.  A global:: qualified name cannot be shadowed.
 */
/**
 * The C# element type for an out-parameter, or "" if it cannot be marshalled as one.
 *
 * Only called for parameters the database flagged PF_is_out, so this does not have to
 * guess: a raw `T *` buffer and an out-parameter are the same pointer by the time they
 * reach here, and the flag is the only thing that tells them apart.
 */
string InterfaceMakerCSharp::
csharp_out_parameter_type(TypeIndex type_index) const {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  TypeIndex resolved = unwrap_type_aliases(type_index);
  if (resolved == 0) {
    return string();
  }
  const InterrogateType &itype = idb->get_type(resolved);
  if (!itype.is_pointer()) {
    return string();
  }
  TypeIndex inner = unwrap_type_aliases(itype.get_wrapped_type());
  if (inner == 0) {
    return string();
  }

  // Numeric only.  bool is not blittable, and an enum would need its underlying
  // type; neither shows up as an out-parameter in practice.
  string element = get_pinvoke_type(inner, false);
  static const char *const numeric[] = {
    "byte", "sbyte", "short", "ushort", "int", "uint",
    "long", "ulong", "float", "double",
  };
  for (const char *candidate : numeric) {
    if (element == candidate) {
      return element;
    }
  }
  return string();
}

string InterfaceMakerCSharp::
globalize_class_name(const InterrogateType &itype) const {
  string name = get_qualified_class_name(itype);
  if (name.compare(0, 8, "global::") == 0) {
    return name;
  }
  // A bare name here is either a genuine current-module type (which we qualify to
  // defeat member-name shadowing) or a dependency type that get_type_module_name
  // misattributes to the current module (get_qualified_class_name returns bare for
  // both, since it treats module==current as "no qualifier needed").  Qualifying the
  // latter with the current module is wrong -- it produces e.g.
  // global::Panda3D.Rplight.LVecBase3f for a Core type, which does not exist.
  //
  // The authoritative test is whether the current module actually emits the type:
  // is_current_native_methods_type consults csharp_owned_type_indices, the set the
  // module writes class files for.  A dependency reached via --search-dir is not in
  // it; its bare name resolves through the file's `using` directives.
  TypeIndex tidx = get_type_index_for_interrogate_type(itype);
  if (tidx != 0 && !_current_module_name.empty() &&
      is_current_native_methods_type(tidx, itype)) {
    return "global::" + prettify_namespace(_current_module_name) + "." + name;
  }
  return name;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_marshal_attribute(CPPType *type, bool for_return) const {
  if (type == nullptr) {
    return string();
  }

  type = TypeManager::resolve_type(type);

  if (TypeManager::is_bool(type)) {
    return for_return ? "[return: MarshalAs(UnmanagedType.I1)]" :
      "[MarshalAs(UnmanagedType.I1)]";
  }

  return string();
}

/**
 *
 */
string InterfaceMakerCSharp::
get_marshal_attribute(TypeIndex type_index, bool for_return) const {
  type_index = unwrap_type_aliases(type_index);
  if (type_index == 0) {
    return string();
  }

  const InterrogateType &itype = InterrogateDatabase::get_ptr()->get_type(type_index);
  if (itype.is_atomic() && itype.get_atomic_token() == AT_bool) {
    return for_return ? "[return: MarshalAs(UnmanagedType.I1)]" :
      "[MarshalAs(UnmanagedType.I1)]";
  }

  return string();
}

/**
 *
 */
string InterfaceMakerCSharp::
get_class_name(const InterrogateType &itype) const {
  if (is_empty_pointer_facade_type(itype)) {
    return get_pointer_facade_target_name(itype);
  }

  return get_csharp_type_name(itype);
}

/**
 * True if this type is nested inside another C++ class that we can emit as a
 * (partial) C# class, so it should be generated as a real nested C# type.
 */
bool InterfaceMakerCSharp::
should_nest_type(const InterrogateType &itype) const {
  TypeIndex outer_idx = itype.get_outer_class();
  if (outer_idx == 0) {
    return false;
  }
  // Only nest genuine classes/structs.  Enums are deliberately NOT nested:
  // Panda pairs almost every nested enum with a same-named accessor (enum Format
  // + get_format() -> property Format), and some share the outer type's own name
  // (ShaderAttrib::ShaderAttrib).  C# forbids a nested type and a member (or the
  // enclosing type) sharing a name (CS0102/CS0542), so nested enums stay
  // top-level (e.g. Texture_Format) and only carry the PascalCase value cleanup.
  if (!(itype.is_class() || itype.is_struct())) {
    return false;
  }
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateType &outer = idb->get_type(outer_idx);
  if (!(outer.is_class() || outer.is_struct())) {
    return false;
  }
  if (should_skip_csharp_type(outer) ||
      is_collection_facade_type(outer) ||
      is_empty_pointer_facade_type(outer)) {
    return false;
  }
  // A nested type may not share the simple name of its enclosing type (CS0542).
  // Compare raw local names directly to avoid recursing back into this helper.
  auto local_name = [](const InterrogateType &t) -> string {
    if (t.has_name()) {
      return make_csharp_identifier(t.get_name());
    }
    if (t.has_scoped_name()) {
      return make_csharp_identifier(InterrogateBuilder::descope(t.get_scoped_name()));
    }
    return string();
  };
  string self_name = local_name(itype);
  if (!self_name.empty() && self_name == local_name(outer)) {
    return false;
  }
  return true;
}

/**
 * Fills `out` with the enclosing classes of a nested type, outermost first
 * (empty for a top-level type).  Stops climbing as soon as an enclosing class
 * is not itself nestable, so every entry is safe to emit as a partial class.
 */
void InterfaceMakerCSharp::
get_outer_class_chain(const InterrogateType &itype,
                      std::vector<const InterrogateType *> &out) const {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  std::vector<const InterrogateType *> inner_first;
  const InterrogateType *cur = &itype;
  while (should_nest_type(*cur)) {
    const InterrogateType &outer = idb->get_type(cur->get_outer_class());
    inner_first.push_back(&outer);
    cur = &outer;
  }
  out.assign(inner_first.rbegin(), inner_first.rend());
}

/**
 * Underscore-free flattened name for a type that has an enclosing class but is
 * NOT emitted as a nested C# type (i.e. enums): AsyncTask::DoneStatus ->
 * AsyncTaskDoneStatus, A::B::C -> ABC.  Top-level types return their plain name.
 */
string InterfaceMakerCSharp::
get_flat_display_name(const InterrogateType &itype) const {
  string simple;
  if (itype.has_name()) {
    simple = make_csharp_identifier(itype.get_name());
  } else if (itype.has_scoped_name()) {
    simple = make_csharp_identifier(InterrogateBuilder::descope(itype.get_scoped_name()));
  } else {
    return get_class_name(itype);
  }
  TypeIndex outer_idx = itype.get_outer_class();
  if (outer_idx == 0) {
    return simple;
  }
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateType &outer = idb->get_type(outer_idx);
  return get_flat_display_name(outer) + simple;
}

/**
 * The innermost simple identifier for a nested type's C# declaration
 * (e.g. "AxisState" for InputDevice::AxisState).  A non-nested type that still
 * has an enclosing class (an enum) uses the underscore-free flattened name
 * (AsyncTaskDoneStatus); a genuine top-level type uses its plain class name.
 */
string InterfaceMakerCSharp::
get_simple_class_name(const InterrogateType &itype) const {
  if (!should_nest_type(itype)) {
    if (itype.get_outer_class() != 0) {
      return get_flat_display_name(itype);
    }
    return get_class_name(itype);
  }
  if (itype.has_name()) {
    return make_csharp_identifier(itype.get_name());
  }
  if (itype.has_scoped_name()) {
    return make_csharp_identifier(InterrogateBuilder::descope(itype.get_scoped_name()));
  }
  return get_class_name(itype);
}

/**
 * The dotted C# reference path for a type (e.g. "InputDevice.AxisState").
 * Enums (non-nested but enclosed) resolve to the underscore-free flattened name;
 * a genuine top-level type resolves to its plain class name.
 */
string InterfaceMakerCSharp::
get_nested_class_name(const InterrogateType &itype) const {
  if (!should_nest_type(itype)) {
    if (itype.get_outer_class() != 0) {
      return get_flat_display_name(itype);
    }
    return get_class_name(itype);
  }
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateType &outer = idb->get_type(itype.get_outer_class());
  return get_nested_class_name(outer) + "." + get_simple_class_name(itype);
}

/**
 * The simple interface identifier for a nested type ("IAxisState"); the full
 * collision-aware interface name for a top-level type.
 */
string InterfaceMakerCSharp::
get_simple_interface_name(const InterrogateType &itype) const {
  if (!should_nest_type(itype)) {
    return get_interface_name(itype);
  }
  return "I" + get_simple_class_name(itype);
}

/**
 * The dotted C# reference path for a type's interface
 * (e.g. "InputDevice.IAxisState").  The nested interface lives inside the
 * enclosing *class*, so the outer path uses the class name.
 */
string InterfaceMakerCSharp::
get_nested_interface_name(const InterrogateType &itype) const {
  if (!should_nest_type(itype)) {
    return get_interface_name(itype);
  }
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateType &outer = idb->get_type(itype.get_outer_class());
  return get_nested_class_name(outer) + "." + get_simple_interface_name(itype);
}

/**
 * Opens a `public partial class Outer {` wrapper for each enclosing class
 * (outermost first) so a nested type can be written as a real C# nested type.
 * Returns the number of wrappers opened; pass it to close_nesting_wrappers.
 */
int InterfaceMakerCSharp::
open_nesting_wrappers(ostream &out, const InterrogateType &itype) const {
  std::vector<const InterrogateType *> chain;
  get_outer_class_chain(itype, chain);
  int level = 1;
  for (const InterrogateType *outer : chain) {
    indent(out, level * 2)
      << "public partial class " << get_simple_class_name(*outer) << " {\n";
    ++level;
  }
  return (int)chain.size();
}

/**
 * Closes the `count` partial-class wrappers opened by open_nesting_wrappers.
 */
void InterfaceMakerCSharp::
close_nesting_wrappers(ostream &out, int count) const {
  for (int i = count; i >= 1; --i) {
    indent(out, i * 2) << "}\n";
  }
}

/**
 *
 */
string InterfaceMakerCSharp::
get_interface_name(const InterrogateType &itype) const {
  string cn = get_class_name(itype);

  string candidate = "I" + cn;

  if (cn.size() >= 2 && cn[0] == 'I' && std::isupper((unsigned char)cn[1])) {
    return "Ifc" + cn;
  }

  // Check for a naming collision with any C++ type whose generated class name
  // equals the candidate interface name.  All databases from the search
  // directories are pre-loaded before writing begins (see load_all_search_dir_databases
  // called from write_csharp_files), so this lookup is reliable regardless of
  // build order.
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  TypeIndex collision_idx = idb->lookup_type_by_scoped_name(candidate);
  if (collision_idx != 0) {
    const InterrogateType &ct = idb->get_type(collision_idx);
    if (ct.is_class() || ct.is_struct()) {
      return candidate + "Ifc";
    }
  }

  // Case-insensitive collision check across all modules.
  if (_all_class_names_lower.empty()) {
    int n = idb->get_num_all_types();
    for (int i = 0; i < n; ++i) {
      TypeIndex tidx = idb->get_all_type(i);
      const InterrogateType &t = idb->get_type(tidx);
      if (t.is_class() || t.is_struct()) {
        string cname = get_class_name(t);
        for (char &c : cname) {
          c = (char)std::tolower((unsigned char)c);
        }
        _all_class_names_lower.insert(cname);
      }
    }
  }
  string candidate_lower = candidate;
  for (char &c : candidate_lower) {
    c = (char)std::tolower((unsigned char)c);
  }
  if (_all_class_names_lower.count(candidate_lower) != 0) {
    return candidate + "Ifc";
  }

  // Fall back to file-existence check for types already generated in this run.
  Filename collision_file(csharp_output_dir.get_fullpath() + "/" + candidate + ".cs");
  if (collision_file.exists()) {
    return candidate + "Ifc";
  }

  return candidate;
}

/**
 * Returns the correct module name for a type, using csharp_library_to_module
 * (from --module-map) when available to override the .in file's module_name
 * (which may be wrong, e.g. "panda3d.net" instead of "panda3d.core").
 */
string InterfaceMakerCSharp::
get_type_module_name(const InterrogateType &itype) const {
  // First: check csharp_type_module_map, which is populated from the command-
  // line .in file range tracking and then snapshotted before search-dir
  // loading.  This preserves the correct module even when search-dir merges
  // later change itype's _def (e.g. MemoryBase stub from p3dtoolbase being
  // overwritten by p3egg's version during core-module search-dir loading).
  TypeIndex tidx = get_type_index_for_interrogate_type(itype);
  if (tidx != 0) {
    auto mit = csharp_type_module_map.find(tidx);
    if (mit != csharp_type_module_map.end() && !mit->second.empty()) {
      return mit->second;
    }
  }
  // Second: use the --module-map library→module lookup if available.
  if (!csharp_library_to_module.empty() && itype.has_library_name()) {
    string lib = itype.get_library_name();
    auto it = csharp_library_to_module.find(lib);
    if (it != csharp_library_to_module.end()) {
      return it->second;
    }
  }
  // Fall back to the .in file's module_name
  if (itype.has_module_name()) {
    return itype.get_module_name();
  }
  return string();
}

string InterfaceMakerCSharp::
get_qualified_class_name(const InterrogateType &itype) const {
  string name = get_nested_class_name(itype);
  string type_module = get_type_module_name(itype);

  if (!_current_module_name.empty() && !type_module.empty() &&
      type_module != _current_module_name) {
    return "global::" + prettify_namespace(type_module) + "." + name;
  }

  return name;
}

string InterfaceMakerCSharp::
get_qualified_interface_name(const InterrogateType &itype) const {
  if (is_collection_facade_type(itype)) {
    return get_qualified_class_name(itype);
  }
  string name = get_nested_interface_name(itype);
  string type_module = get_type_module_name(itype);

  if (!_current_module_name.empty() && !type_module.empty() &&
      type_module != _current_module_name) {
    return "global::" + prettify_namespace(type_module) + "." + name;
  }

  return name;
}

bool InterfaceMakerCSharp::
uses_csharp_interface(const InterrogateType &itype) const {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  int type_count = idb->get_num_all_types();
  if (!_csharp_interface_cache_valid ||
      _csharp_interface_cache_type_count != type_count) {
    rebuild_csharp_interface_cache();
  }

  TypeIndex type_index = get_type_index_for_interrogate_type(itype);
  return type_index != 0 &&
         _csharp_interface_type_indices.find(type_index) != _csharp_interface_type_indices.end();
}

bool InterfaceMakerCSharp::
mark_csharp_interface_base_chain(TypeIndex type_index,
                                 std::set<TypeIndex> &visited) const {
  type_index = unwrap_type_aliases(type_index);
  if (type_index == 0 || !visited.insert(type_index).second) {
    return false;
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateType &itype = idb->get_type(type_index);
  if (!(itype.is_class() || itype.is_struct())) {
    return false;
  }

  _csharp_interface_type_indices.insert(type_index);

  int num_derivations = itype.number_of_derivations();
  for (int di = 0; di < num_derivations; ++di) {
    if (itype.derivation_is_pointer_to(di)) {
      continue;
    }
    TypeIndex base_index = itype.get_derivation(di);
    if (base_index != 0 && is_wrapped_type(base_index)) {
      mark_csharp_interface_base_chain(base_index, visited);
    }
  }

  return true;
}

void InterfaceMakerCSharp::
rebuild_csharp_interface_cache() const {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  _csharp_interface_type_indices.clear();

  int type_count = idb->get_num_all_types();
  for (int i = 0; i < type_count; ++i) {
    TypeIndex type_index = idb->get_all_type(i);
    if (type_index == 0) {
      continue;
    }

    const InterrogateType &itype = idb->get_type(type_index);
    if (!(itype.is_class() || itype.is_struct())) {
      continue;
    }

    if (itype.has_forced_complex_inheritance()) {
      _csharp_interface_type_indices.insert(type_index);
    }

    int real_derivations = 0;
    int num_derivations = itype.number_of_derivations();
    for (int di = 0; di < num_derivations; ++di) {
      if (itype.derivation_is_pointer_to(di)) {
        continue;
      }
      TypeIndex base_index = itype.get_derivation(di);
      if (base_index != 0 && is_wrapped_type(base_index)) {
        ++real_derivations;
      }
    }

    if (real_derivations > 1) {
      _csharp_interface_type_indices.insert(type_index);
      std::set<TypeIndex> visited;
      for (int di = 0; di < num_derivations; ++di) {
        if (itype.derivation_is_pointer_to(di)) {
          continue;
        }
        TypeIndex base_index = itype.get_derivation(di);
        if (base_index != 0 && is_wrapped_type(base_index)) {
          mark_csharp_interface_base_chain(base_index, visited);
        }
      }
    }
  }

  _csharp_interface_cache_type_count = type_count;
  _csharp_interface_cache_valid = true;
}

string InterfaceMakerCSharp::
get_public_native_object_type_name(const InterrogateType &itype,
                                   bool for_return) const {
  string result = uses_csharp_interface(itype)
    ? get_qualified_interface_name(itype)
    : get_qualified_class_name(itype);
  if (for_return) {
    result += "?";
  }
  return result;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_base_class_clause(const InterrogateType &itype) const {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();

  // Walk the primary derivation chain to find the first base that is both
  // wrapped and not an unexported C++ internal type (e.g. MemoryBase).
  const InterrogateType *current = &itype;
  for (int depth = 0; depth < 32; ++depth) {
    if (current->number_of_derivations() == 0) {
      break;
    }
    // A DF_pointer_to derivation means "holds a T", not "is a T" — it is the
    // synthesized edge from a smart-pointer holder (PointerToBase<T>) to its
    // pointee, not inheritance.  Turning it into a C# base class was always
    // wrong; it merely happened to compile while the pointee was an ordinary
    // class.  It stops compiling the moment the pointee is a collection facade,
    // because those are sealed.
    if (current->derivation_is_pointer_to(0)) {
      break;
    }
    TypeIndex base_index = current->get_derivation(0);
    if (!is_wrapped_type(base_index)) {
      break;
    }
    const InterrogateType &base_type = idb->get_type(base_index);
    if (!should_skip_csharp_type(base_type)) {
      return get_qualified_class_name(base_type);
    }
    // Base is an unexported internal type — keep climbing.
    current = &base_type;
  }
  return "NativeObject";
}

/**
 *
 */
string InterfaceMakerCSharp::
get_interface_list(const InterrogateType &itype) const {
  // Collect ALL secondary bases from the entire primary-base ancestry chain.
  // C# explicit interface implementations don't inherit, so each class must
  // declare every secondary base interface it implements, not just direct ones.
  std::set<const InterrogateType *> all_secondary_bases;
  std::set<string> emitted_interfaces;
  string interface_list;

  std::function<void(const InterrogateType &)> collect;
  collect = [&](const InterrogateType &t) {
    std::vector<const InterrogateType *> secondaries;
    get_secondary_base_types(t, secondaries);
    for (const InterrogateType *sec : secondaries) {
      if (sec != nullptr && all_secondary_bases.insert(sec).second) {
        collect(*sec);
      }
    }
    if (t.number_of_derivations() > 0) {
      TypeIndex primary_idx = t.get_derivation(0);
      if (is_wrapped_type(primary_idx)) {
        InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
        const InterrogateType &primary = idb->get_type(primary_idx);
        collect(primary);
      }
    }
  };
  collect(itype);

  for (const InterrogateType *base_type : all_secondary_bases) {
    if (is_collection_facade_type(*base_type) ||
        !uses_csharp_interface(*base_type)) {
      continue;
    }

    string interface_name = get_qualified_interface_name(*base_type);
    if (emitted_interfaces.insert(interface_name).second) {
      interface_list += ", " + interface_name;
    }
  }

  return interface_list;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_interface_base_list(const InterrogateType &itype) const {
  std::set<string> emitted_interfaces;
  string interface_list = " : INativeObject";

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();

  auto has_members = [](const InterrogateType &t) {
    return t.number_of_methods() != 0 ||
           t.number_of_casts() != 0 ||
           t.number_of_elements() != 0;
  };

  int num_derivations = itype.number_of_derivations();
  for (int di = 0; di < num_derivations; ++di) {
    TypeIndex base_index = itype.get_derivation(di);
    if (base_index == 0 || !is_wrapped_type(base_index)) {
      continue;
    }

    const InterrogateType &base_type = idb->get_type(base_index);
    if (is_collection_facade_type(base_type)) {
      continue;
    }
    if (!uses_csharp_interface(base_type)) {
      continue;
    }

    // Skip C++ internal types that are not exported for scripting (e.g.
    // MemoryBase, _object).  Without this check, IReferenceCount would
    // inherit from IMemoryBase which has no .cs file, causing CS0246.
    if (should_skip_csharp_type(base_type)) {
      continue;
    }

    // For secondary bases (di > 0), only include the interface if the base
    // actually has methods available.  A secondary base whose methods can't be
    // resolved (e.g. a cross-module stub with a wrong library_name, like
    // Namable referenced by EggNamedObject) would produce CS0535 because the
    // implementing class can't satisfy the inherited interface contract.
    if (di > 0) {
      const InterrogateType *effective = &base_type;

      // Try lazy database loading if the type has no members yet.
      if (!has_members(*effective)) {
        ensure_database_loaded(*effective);
        effective = &idb->get_type(base_index);
      }

      // Try scoped-name lookup in case a richer version is in a loaded db.
      if (!has_members(*effective) && effective->has_scoped_name()) {
        TypeIndex resolved = idb->lookup_type_by_scoped_name(effective->get_scoped_name());
        if (resolved != 0) {
          const InterrogateType &rt = idb->get_type(resolved);
          if (has_members(rt)) {
            effective = &rt;
          }
        }
      }

      if (!has_members(*effective)) {
        continue;  // Can't implement this interface – skip it.
      }
    }

    string interface_name = get_qualified_interface_name(base_type);
    if (emitted_interfaces.insert(interface_name).second) {
      interface_list += ", " + interface_name;
    }
  }

  return interface_list;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_collection_interface_type(const InterrogateType &itype, bool for_return) const {
  string element_type = get_collection_element_type(itype, true);
  CollectionFacadeKind kind = get_collection_facade_kind(itype);
  string iface = (kind == CF_mutable_array) ? "IList<" : "IReadOnlyList<";
  iface += element_type + ">";
  if (for_return) {
    iface += "?";
  }
  return iface;
}

/**
 * Maps a C++ element true_name (as it appears inside std::vector< ... >) to
 * the C# type name to use in generated code.
 *
 * `for_signature=true` returns the interface name (e.g. IFoo) for
 * user-defined types, used in public interface signatures.  `false`
 * returns the concrete class name (e.g. Foo).
 *
 * Handles the usual primitives plus std::string/std::wstring.  For
 * anything else we look the name up in the database (by true_name, falling
 * back to scoped name) and return the associated C# class/interface name.
 */
string InterfaceMakerCSharp::
get_collection_element_type_from_cpp_name(const string &cpp_name, bool for_signature) const {
  // Normalise whitespace that cppparser sometimes leaves: "std::vector< int >"
  // gives us " int ".  Trim surrounding spaces.
  string clean = cpp_name;
  while (!clean.empty() && (clean.front() == ' ' || clean.front() == '\t')) clean.erase(clean.begin());
  while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\t')) clean.pop_back();

  // Primitive / string mappings.  These must match get_pinvoke_type's
  // behaviour so Count/Item pinvokes line up with the helper declarations.
  if (clean == "unsigned char") return "byte";
  if (clean == "signed char") return "sbyte";
  if (clean == "char") return "sbyte";
  if (clean == "unsigned short" || clean == "unsigned short int") return "ushort";
  if (clean == "short" || clean == "short int") return "short";
  if (clean == "unsigned int" || clean == "unsigned") return "uint";
  if (clean == "int") return "int";
  if (clean == "long" || clean == "long int") return "int";
  if (clean == "unsigned long" || clean == "unsigned long int") return "uint";
  if (clean == "long long" || clean == "long long int") return "long";
  if (clean == "unsigned long long" || clean == "unsigned long long int") return "ulong";
  if (clean == "float") return "float";
  if (clean == "double") return "double";
  if (clean == "bool") return "bool";
  if (clean == "std::string" || clean == "string" ||
      clean == "std::wstring" || clean == "wstring") return "string";

  // A PT(T) element surfaces as T.
  {
    string pointee = collection_element_pointee(clean);
    if (!pointee.empty()) {
      return get_collection_element_type_from_cpp_name(pointee, for_signature);
    }
  }

  // Fall back to a database lookup for user-defined types.
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  TypeIndex type_index = idb->lookup_type_by_true_name(clean);
  if (type_index == 0) {
    type_index = idb->lookup_type_by_scoped_name(clean);
  }
  if (type_index == 0) {
    type_index = idb->lookup_type_by_name(clean);
  }
  if (type_index != 0) {
    type_index = unwrap_type_aliases(type_index);
    if (type_index != 0) {
      const InterrogateType &itype = idb->get_type(type_index);
      if (for_signature && uses_csharp_interface(itype)) {
        return get_interface_name(itype);
      }
      // Qualified, not the bare class name: a nested type is emitted as a nested
      // C# class, so InputDevice::ButtonState is `InputDevice.ButtonState`.  The
      // flat form (`InputDevice_ButtonState`) names only the *file*, and emitting
      // it here produced a NativeList<> over a type that does not exist.
      return get_qualified_class_name(itype);
    }
  }

  // Last resort: mangle whatever we got into an identifier.
  return make_csharp_identifier(clean);
}

/**
 * Returns the element type's C# name for a collection facade type.
 * Internally walks the inheritance chain to find the template
 * instantiation (e.g. std::vector<T>) and extracts T.
 */
string InterfaceMakerCSharp::
get_collection_element_type(const InterrogateType &itype, bool for_signature) const {
  string element_cpp_name;
  if (detect_collection_facade_kind(itype, element_cpp_name) == CF_none) {
    return string();
  }
  return get_collection_element_type_from_cpp_name(element_cpp_name, for_signature);
}

/**
 * Like get_collection_element_type but returns the raw C++ element name
 * (e.g. "int", "std::string") for use inside generated C++ helpers.
 */
string InterfaceMakerCSharp::
get_collection_element_cpp_type(const InterrogateType &itype) const {
  string element_cpp_name;
  if (detect_collection_facade_kind(itype, element_cpp_name) == CF_none) {
    return string();
  }
  while (!element_cpp_name.empty() && (element_cpp_name.front() == ' ' || element_cpp_name.front() == '\t')) element_cpp_name.erase(element_cpp_name.begin());
  while (!element_cpp_name.empty() && (element_cpp_name.back() == ' ' || element_cpp_name.back() == '\t')) element_cpp_name.pop_back();
  return element_cpp_name;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_collection_canonical_library(const InterrogateType &itype) const {
  // Deterministic owner library for a collection facade class name: the min
  // library over all facade entries sharing the name.  Unlike
  // itype.get_library_name() (the volatile post-merge _def winner, which flips
  // when a second module introduces the collection and differs per process),
  // this is stable across modules and rebuilds, so the facade hash, declaration
  // hash, and owner module all agree.  Each candidate is a definer whose pass-1
  // run exported Collection_<hash(lib)>_..., so the chosen hash always resolves.
  string key = get_class_name(itype);
  auto cached = csharp_collection_canonical_library.find(key);
  if (cached != csharp_collection_canonical_library.end()) {
    return cached->second;
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  string best;
  int n = idb->get_num_all_types();
  for (int t = 0; t < n; ++t) {
    const InterrogateType &cand = idb->get_type(idb->get_all_type(t));
    if (!cand.has_library_name() || !is_collection_facade_type(cand)) {
      continue;
    }
    if (get_class_name(cand) != key) {
      continue;
    }
    string lib = cand.get_library_name();
    if (!lib.empty() && (best.empty() || lib < best)) {
      best = lib;
    }
  }

  csharp_collection_canonical_library[key] = best;
  return best;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_collection_helper_name(const InterrogateType &itype, const string &op) const {
  // Use the class name (e.g. "vector_int") as the stable part — it's
  // unique within a module and agrees across pass 1 and pass 2, unlike
  // TypeIndex which can shift.  Prefix with the owning library's hash
  // to disambiguate modules that both emit helpers for "vector_string"
  // (mirrors the `_inC<hash><remap_hash>` scheme in get_c_wrapper_name).
  //
  // The hash must come from the type's own library, not the currently
  // emitting C# library: in pass 2 multiple native modules merge into
  // one C# assembly, so `_def->library_hash_name` wouldn't match the
  // pass-1 export.
  std::ostringstream strm;
  strm << "Collection_";
  string lib;
  if (csharp_database_only_pass) {
    // Pass 2: one canonical owner, so the facade and its declaration never
    // disagree on the hash (see get_collection_canonical_library).
    lib = get_collection_canonical_library(itype);
  }
  if (lib.empty() && itype.has_library_name()) {
    // Pass 1: the library's own hash (the canonical pick is always one of these).
    const char *l = itype.get_library_name();
    if (l != nullptr && *l != '\0') {
      lib = l;
    }
  }
  if (!lib.empty()) {
    strm << InterrogateBuilder::hash_string(lib, 5) << "_";
  } else if (_def != nullptr && _def->library_hash_name != nullptr) {
    strm << _def->library_hash_name << "_";
  }
  strm << get_class_name(itype) << "_" << op;
  return strm.str();
}

/**
 * Returns 0 if no inherited member with the same signature exists, 1 if an
 * inherited member exists but cannot be overridden due to managed return-type
 * incompatibility, and 2 if the inherited member is override-compatible.
 * Generated C# instance methods are all virtual, but C# still requires matching
 * return types for overrides; incompatible hiders are emitted with `new`.
 * When primary_chain_only is true, only the primary C# class chain is
 * considered; otherwise, the full wrapped interface-base closure is checked.
 */
int InterfaceMakerCSharp::
inherited_method_signature_kind(const InterrogateType &itype,
                                const string &method_name,
                                const std::vector<string> &param_types,
                                const string &return_type,
                                bool is_static,
                                bool primary_chain_only) {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  std::set<TypeIndex> seen;
  string target_sig = build_signature_key(method_name, param_types, is_static);

  auto make_remap_parameter_types = [&](FunctionRemap *remap) {
    std::vector<string> result;
    size_t first_param = remap->_has_this ? 1 : 0;
    for (size_t i = first_param; i < remap->_parameters.size(); ++i) {
      ParameterRemap *param_remap = remap->_parameters[i]._remap;
      TypeIndex param_type_index = get_parameter_type_for_remap(remap, i);
      bool param_nullable = is_parameter_nullable(remap, i);
      string param_type = (param_type_index != 0)
        ? get_csharp_signature_type(param_type_index, param_nullable, /*is_parameter=*/true)
        : get_csharp_signature_type_for_wrapper(param_remap, param_nullable);

      AtomicToken stream_tok = csharp_stream_token_for_type(param_type_index);
      if (stream_tok != AT_not_atomic) {
        param_type = "global::System.IO.Stream";
      }

      result.push_back(param_type);
    }
    return result;
  };

  auto make_wrapper_parameter_types = [&](const InterrogateFunctionWrapper &wrapper,
                                         bool wrapper_static) {
    std::vector<string> result;
    int first_param = wrapper_static ? 0 : 1;
    for (int i = first_param; i < wrapper.number_of_parameters(); ++i) {
      TypeIndex param_type_index = wrapper.parameter_get_type(i);
      string param_type = get_csharp_signature_type(param_type_index,
                                                    wrapper.parameter_is_nullable(i),
                                                    /*is_parameter=*/true);

      AtomicToken stream_tok = csharp_stream_token_for_type(param_type_index);
      if (stream_tok != AT_not_atomic) {
        param_type = "global::System.IO.Stream";
      }

      result.push_back(param_type);
    }
    return result;
  };

  auto make_remap_return_type = [&](FunctionRemap *remap) {
    TypeIndex return_type_index = get_return_type_for_remap(remap);
    bool return_nullable = is_return_nullable(remap);
    return remap->_void_return ? string("void") :
      ((return_type_index != 0)
         ? get_csharp_signature_type(return_type_index, return_nullable)
         : get_csharp_signature_type_for_wrapper(remap->_return_type, return_nullable));
  };

  auto make_wrapper_return_type = [&](const InterrogateFunctionWrapper &wrapper) {
    return wrapper.has_return_value()
      ? get_csharp_signature_type(wrapper.get_return_type(), wrapper.is_return_nullable())
      : string("void");
  };

  auto inherited_kind_for_return = [&](const string &inherited_return_type) {
    return inherited_return_type == return_type ? 2 : 1;
  };

  std::function<int(const InterrogateType &)> visit = [&](const InterrogateType &current) {
    int num_derivations = current.number_of_derivations();
    for (int di = 0; di < num_derivations; ++di) {
      if (primary_chain_only && di != 0) {
        break;
      }

      TypeIndex base_index = current.get_derivation(di);
      if (base_index == 0 || !is_wrapped_type(base_index) || !seen.insert(base_index).second) {
        continue;
      }

      const InterrogateType &base_type = idb->get_type(base_index);
      auto inspect_methods_on_type = [&](const InterrogateType &candidate) -> int {
        for (int mi = 0; mi < candidate.number_of_methods(); ++mi) {
          Function *method = record_function(candidate, candidate.get_method(mi));
          if (method == nullptr) {
            continue;
          }

          string inherited_name = make_csharp_identifier(method->_ifunc.get_name());
          string inherited_alias = to_pascal_case(inherited_name);
          if (method_name != inherited_name && method_name != inherited_alias) {
            continue;
          }

          for (FunctionRemap *remap : method->_remaps) {
            if (remap == nullptr || (remap->_flags & FunctionRemap::F_explicit_self) != 0 ||
                remap->_type == FunctionRemap::T_constructor ||
                remap->_type == FunctionRemap::T_destructor ||
                !is_remap_legal_csharp(remap)) {
              continue;
            }

            bool remap_static = !remap->_has_this;
            if (remap_static != is_static) {
              continue;
            }

            std::vector<string> inherited_param_types = make_remap_parameter_types(remap);
            if (build_signature_key(method_name, inherited_param_types, remap_static) == target_sig) {
              return inherited_kind_for_return(make_remap_return_type(remap));
            }
          }

          if (method->_remaps.empty()) {
            int num_wrappers = method->_ifunc.number_of_c_wrappers();
            for (int wi = 0; wi < num_wrappers; ++wi) {
              FunctionWrapperIndex wrapper_index = method->_ifunc.get_c_wrapper(wi);
              if (wrapper_index == 0) {
                continue;
              }
              const InterrogateFunctionWrapper &wrapper = idb->get_wrapper(wrapper_index);
              if (!is_wrapper_legal_csharp(wrapper)) {
                continue;
              }

              bool wrapper_static = !(wrapper.number_of_parameters() != 0 && wrapper.parameter_is_this(0));
              if (wrapper_static != is_static) {
                continue;
              }

              std::vector<string> inherited_param_types =
                make_wrapper_parameter_types(wrapper, wrapper_static);
              if (build_signature_key(method_name, inherited_param_types, wrapper_static) == target_sig) {
                return inherited_kind_for_return(make_wrapper_return_type(wrapper));
              }
            }
          }
        }

        return 0;
      };

      int direct_inherited = inspect_methods_on_type(base_type);
      if (direct_inherited == 0) {
        std::vector<const InterrogateType *> secondary_base_types;
        get_secondary_base_types(base_type, secondary_base_types);
        for (const InterrogateType *secondary : secondary_base_types) {
          if (secondary == nullptr) {
            continue;
          }
          direct_inherited = inspect_methods_on_type(*secondary);
          if (direct_inherited != 0) {
            break;
          }
        }
      }
      if (direct_inherited != 0) {
        return direct_inherited;
      }

      int inherited = visit(base_type);
      if (inherited != 0) {
        return inherited;
      }
    }

    return 0;
  };

  return visit(itype);
}

/**
 *
 */
bool InterfaceMakerCSharp::
is_wrapped_type(TypeIndex type_index) const {
  if (type_index == 0) {
    return false;
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateType &itype = idb->get_type(type_index);
  return itype.is_class() || itype.is_struct();
}

/**
 *
 */
bool InterfaceMakerCSharp::
type_has_destructor(Object *object) const {
  return !get_destructor_wrapper_name(object).empty();
}

/**
 *
 */
string InterfaceMakerCSharp::
get_destructor_wrapper_name(Object *object) const {
  if (object == nullptr || !object->_itype.has_destructor()) {
    return string();
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateFunction &ifunc = idb->get_function(object->_itype.get_destructor());

  if (ifunc.number_of_c_wrappers() != 0) {
    const InterrogateFunctionWrapper &wrapper = idb->get_wrapper(ifunc.get_c_wrapper(0));
    return make_csharp_identifier("Destroy_" + get_class_name(object->_itype) +
                                  "_" + wrapper.get_unique_name());
  }

  return get_supplemental_destructor_name(object->_itype);
}
