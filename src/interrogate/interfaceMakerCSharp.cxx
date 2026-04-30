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
#include <functional>
#include <fstream>
#include <set>
#include <sstream>

using std::ostream;
using std::string;

std::set<int> csharp_owned_type_indices;
std::map<int, std::string> csharp_type_module_map;
std::map<std::string, std::string> csharp_library_to_module;

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
    InterfaceMaker::indent(out, indent_level)
      << "static std::string string_holder = TextEncoder::encode_wtext("
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

  if (itype.has_name()) {
    return make_csharp_identifier(itype.get_name());
  }
  if (itype.has_scoped_name()) {
    return make_csharp_identifier(InterrogateBuilder::descope(itype.get_scoped_name()));
  }
  if (itype._cpptype != nullptr) {
    return make_csharp_identifier(itype._cpptype->get_local_name(&parser));
  }

  return "UnnamedType";
}

enum CollectionFacadeKind {
  CF_none,
  CF_array_base,
  CF_readonly_array,
  CF_mutable_array
};

static CollectionFacadeKind get_collection_facade_kind_from_name(const string &name) {
  if (name.compare(0, 20, "ConstPointerToArray_") == 0) return CF_readonly_array;
  if (name.compare(0, 15, "PointerToArray_") == 0) return CF_mutable_array;
  if (name.compare(0, 8, "pvector_") == 0) return CF_mutable_array;
  if (name.compare(0, 7, "vector_") == 0 && name.find("iterator") == string::npos) return CF_mutable_array;
  if (name.compare(0, 15, "ConstPointerTo_") == 0) {
    if (name.find("Array") != string::npos || name.find("vector") != string::npos) return CF_array_base;
  }
  if (name.compare(0, 10, "PointerTo_") == 0) {
    if (name.find("Array") != string::npos || name.find("vector") != string::npos) return CF_array_base;
  }
  return CF_none;
}

static CollectionFacadeKind get_collection_facade_kind(const InterrogateType &itype) {
  string cn = get_csharp_type_name(itype);
  if (cn.empty()) return CF_none;
  return get_collection_facade_kind_from_name(cn);
}

static bool is_collection_facade_type(const InterrogateType &itype) {
  return get_collection_facade_kind(itype) != CF_none;
}

static TypeIndex unwrap_type_aliases(TypeIndex type_index);

static bool is_collection_type_index(TypeIndex type_index) {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  if (type_index == 0) return false;
  const InterrogateType &itype = idb->get_type(type_index);
  if (is_collection_facade_type(itype)) return true;
  // Also check through pointer/reference/const/alias wrapping
  if (itype.is_pointer() || itype.is_wrapped()) {
    TypeIndex inner = unwrap_type_aliases(itype.get_wrapped_type());
    if (inner != 0 && inner != type_index) {
      return is_collection_type_index(inner);
    }
  }
  return false;
}

static string get_collection_suffix(const string &name) {
  if (name.compare(0, 20, "ConstPointerToArray_") == 0) return name.substr(20);
  if (name.compare(0, 15, "PointerToArray_") == 0) return name.substr(15);
  if (name.compare(0, 15, "ConstPointerTo_") == 0) return name.substr(15);
  if (name.compare(0, 10, "PointerTo_") == 0) return name.substr(10);
  if (name.compare(0, 8, "pvector_") == 0) return name.substr(8);
  if (name.compare(0, 7, "vector_") == 0) return name.substr(7);
  return name;
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
    return "NativeObject.Unwrap(" + param_name + ")";
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
  return itype.is_atomic() && itype.get_atomic_token() == AT_char;
}

string
get_pinvoke_atomic_type(const InterrogateType &itype) {
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
    return "string";
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
  _dll_name(csharp_dll_name)
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
    if (facade_kind != CF_none && facade_kind != CF_array_base) {
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

  for (Object *object : collection_objects) {
    const InterrogateType &itype = object->_itype;
    CollectionFacadeKind facade_kind = get_collection_facade_kind(itype);
    if (facade_kind == CF_none || facade_kind == CF_array_base) continue;

    bool is_mutable = (facade_kind == CF_mutable_array);
    string helper_prefix = get_collection_helper_name(itype, "");
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

    string element_type_value = get_collection_element_type_from_suffix(get_collection_suffix(get_csharp_type_name(itype)), false);
    bool is_string = (element_type_value == "string");
    bool is_primitive = is_csharp_primitive_type(element_type_value);
    bool is_blittable = is_csharp_blittable_type(element_type_value);
    string e_cpp_type = base_cpp_type + "::value_type";

    // Const-qualified types are read-only: suppress mutable helpers.
    if (is_const_type) {
      is_mutable = false;
    }

    // Use base_cpp_type (no const) for allocation: "new const T()" returns
    // const T* which cannot convert to void*.
    out << "EXPORT_FUNC void *" << helper_prefix << "empty_constructor() { return new " << base_cpp_type << "(); }\n";
    out << "EXPORT_FUNC int " << helper_prefix << "size(" << cpp_type << " *self) { return self->size(); }\n";
    out << "EXPORT_FUNC ";
    if (is_string) {
      out << "const char *" << helper_prefix << "get_element(" << cpp_type << " *self, int index) { return (*self)[index].c_str(); }\n";
    } else if (is_primitive) {
      out << e_cpp_type << " " << helper_prefix << "get_element(" << cpp_type << " *self, int index) { return (*self)[index]; }\n";
    } else {
      out << "void *" << helper_prefix << "get_element(" << cpp_type << " *self, int index) { return new " << e_cpp_type << "((*self)[index]); }\n";
    }

    if (is_mutable) {
      out << "EXPORT_FUNC void " << helper_prefix << "set_element(" << cpp_type << " *self, int index, ";
      if (is_string) {
        out << "const char *val) { (*self)[index] = val; }\n";
      } else if (is_primitive) {
        out << e_cpp_type << " val) { (*self)[index] = val; }\n";
      } else {
        out << e_cpp_type << " *val) { (*self)[index] = *val; }\n";
      }
      out << "EXPORT_FUNC void " << helper_prefix << "push_back(" << cpp_type << " *self, ";
      if (is_string) {
        out << "const char *val) { self->push_back(val); }\n";
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
      if (itype.has_library_name()) {
        string lib = itype.get_library_name();
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

  for (oi = _objects.begin(); oi != _objects.end(); ++oi) {
    Object *object = (*oi).second;
    if (object == nullptr || should_skip_csharp_type(object->_itype)) {
      continue;
    }
    const InterrogateType &itype = object->_itype;
    CollectionFacadeKind facade_kind = get_collection_facade_kind(itype);
    if (facade_kind == CF_none || facade_kind == CF_array_base) continue;

    bool is_mutable = (facade_kind == CF_mutable_array);
    string helper_prefix = get_collection_helper_name(itype, "");
    string element_type_value = get_collection_element_type_from_suffix(get_collection_suffix(get_csharp_type_name(itype)), false);
    bool is_string = (element_type_value == "string");
    bool is_primitive = is_csharp_primitive_type(element_type_value);
    bool is_blittable = is_csharp_blittable_type(element_type_value);

    string cs_ret_type = is_string ? "string" : (is_primitive ? element_type_value : "IntPtr");
    string cs_param_type = is_string ? "string" : (is_primitive ? element_type_value : "IntPtr");

    out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"" << helper_prefix << "empty_constructor\")]\n";
    out << "    internal static partial IntPtr " << helper_prefix << "empty_constructor();\n\n";

    out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"" << helper_prefix << "size\")]\n";
    out << "    internal static partial int " << helper_prefix << "size(IntPtr self);\n\n";

    out << "    [LibraryImport(\"" << quote_csharp_string(_dll_name) << "\", EntryPoint = \"" << helper_prefix << "get_element\"";
    if (is_string) out << ", StringMarshalling = StringMarshalling.Utf8";
    out << ")]\n";
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

      string enum_name = get_class_name(itype);
      if (!_written_enums.insert(enum_name).second) {
        continue;
      }

      std::ofstream out;
      if (!open_output_file(dir, enum_name + ".cs", out)) {
        continue;
      }

      out << "namespace " << cs_namespace << " {\n";
      write_enum_type(out, itype);
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

    string enum_name = get_class_name(itype);
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
    write_enum_type(out, itype);
    out << "}\n";
  }
}

/**
 *
 */
void InterfaceMakerCSharp::
write_enum_type(ostream &out, const InterrogateType &itype) {
  string enum_name = get_class_name(itype);

  out << "  public enum " << enum_name << " : int {\n";
  for (int i = 0; i < itype.number_of_enum_values(); ++i) {
    out << "    " << make_csharp_identifier(itype.get_enum_value_name(i))
        << " = " << itype.get_enum_value(i);
    if (i + 1 < itype.number_of_enum_values()) {
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

  write_interface(out, object);
  out << "\n";
  write_proxy_class(out, cs_namespace, object);
  out << "}\n";
}

/**
 *
 */
void InterfaceMakerCSharp::
write_interface(ostream &out, Object *object) {
  string interface_name = get_interface_name(object->_itype);
  bool is_collection_facade = is_collection_facade_type(object->_itype);

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  const InterrogateType &itype = object->_itype;

  int num_elements = itype.number_of_elements();

  out << "  " << (is_collection_facade ? "internal" : "public") << " interface " << interface_name
      << get_interface_base_list(itype) << " {\n";

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
      write_property(out, &property, object, 4, true);
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
  string element_type = get_collection_element_type_from_suffix(get_collection_suffix(class_name), true);
  string element_value_type = get_collection_element_type_from_suffix(get_collection_suffix(class_name), false);
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

  if (facade_kind == CF_array_base) {
    return;
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

  if (indexer_length_remap == nullptr && indexer_length_wrapper == nullptr &&
      indexer_element_remap == nullptr && indexer_element_wrapper == nullptr) {
    use_collection_helpers = true;
  }

  out << "  public sealed partial class " << class_name << " : Interrogate."
      << (is_mutable ? "NativeList<" : "NativeReadOnlyList<") << element_type << ">, INativeType<"
      << class_name << "> {\n";

  indent(out, 4) << "public static " << class_name
                 << "? __CreateFromNative(IntPtr ptr, NativeOwnership own) {\n";
  indent(out, 6) << "return ptr == IntPtr.Zero ? null : new " << class_name << "(ptr, own);\n";
  indent(out, 4) << "}\n\n";

  if (use_collection_helpers) {
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

  if (is_mutable && use_collection_helpers) {
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
    string element_type_value = get_collection_element_type_from_suffix(get_collection_suffix(get_csharp_type_name(itype)), false);
    if (element_type_value == "string") {
      indent(out, 6) << "return " << native_call << "!;\n";
    } else if (is_csharp_primitive_type(element_type_value)) {
      indent(out, 6) << "return " << native_call << ";\n";
    } else {
      indent(out, 6) << "IntPtr result = " << native_call << ";\n";
      indent(out, 6) << "return " << element_value_type << ".__CreateFromNative(result, NativeOwnership.Owned)"
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
      string element_type_value = get_collection_element_type_from_suffix(get_collection_suffix(get_csharp_type_name(itype)), false);
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
      string element_type_value = get_collection_element_type_from_suffix(get_collection_suffix(get_csharp_type_name(itype)), false);
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

/**
 *
 */
void InterfaceMakerCSharp::
write_proxy_class(ostream &out, const string &, Object *object) {
  const InterrogateType &itype = object->_itype;
  string class_name = get_class_name(itype);
  string base_class = get_base_class_clause(itype);
  string interface_list = get_interface_list(itype);
  string destructor_name = get_destructor_wrapper_name(object);
  string opaque_class_name = "__Opaque_" + class_name;
  bool is_collection_facade = is_collection_facade_type(itype);

  MakeSeq *indexer_make_seq = nullptr;
  FunctionRemap *indexer_length_remap = nullptr;
  FunctionRemap *indexer_element_remap = nullptr;
  string indexer_type;
  for (MakeSeq *make_seq : object->_make_seqs) {
    if (make_seq == nullptr || make_seq->_length_getter == nullptr ||
        make_seq->_element_getter == nullptr) {
      continue;
    }

    FunctionRemap *length_remap = nullptr;
    FunctionRemap *element_remap = nullptr;
    {
      size_t min_lp = 999, min_ep = 999;
      for (auto ri = make_seq->_length_getter->_remaps.begin();
           ri != make_seq->_length_getter->_remaps.end(); ++ri) {
        FunctionRemap *r = *ri;
        if (r != nullptr && is_remap_legal_csharp(r) && r->_parameters.size() < min_lp) {
          min_lp = r->_parameters.size();
          length_remap = r;
        }
      }
      for (auto ri = make_seq->_element_getter->_remaps.begin();
           ri != make_seq->_element_getter->_remaps.end(); ++ri) {
        FunctionRemap *r = *ri;
        if (r == nullptr || !is_remap_legal_csharp(r) || r->_void_return) continue;
        size_t eself = r->_has_this ? 1 : 0;
        size_t eextra = r->_parameters.size() - eself;
        if (eextra >= 1 && r->_parameters.size() < min_ep) {
          min_ep = r->_parameters.size();
          element_remap = r;
        }
      }
    }
    if (length_remap == nullptr || element_remap == nullptr) {
      continue;
    }

    if (element_remap->_void_return ||
        !is_remap_legal_csharp(length_remap) ||
        !is_remap_legal_csharp(element_remap)) {
      continue;
    }

    indexer_make_seq = make_seq;
    indexer_length_remap = length_remap;
    indexer_element_remap = element_remap;
    indexer_type = get_csharp_type_for_wrapper(element_remap->_return_type, is_return_nullable(element_remap));
    break;
  }

  bool is_abstract = is_abstract_type(itype);
  std::vector<const InterrogateType *> secondary_base_types;
  get_secondary_base_types(itype, secondary_base_types);

  if (itype.has_comment()) {
    emit_xml_doc_comment(out, itype.get_comment(), 2);
  }

  out << "  " << (is_collection_facade ? "internal " : "public ");
  if (is_abstract) {
    out << "abstract ";
  }
  out << "partial class " << class_name << " : " << base_class
      << ", " << get_interface_name(itype)
      << ", INativeType<" << class_name << ">" << interface_list;
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

  indent(out, 4) << "internal " << class_name
                 << "(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {\n";
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
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
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
        write_property(out, &property, object, 4, false);
      }
    }
  };

  emit_properties_for_type(itype);
  for (const InterrogateType *base_type : secondary_base_types) {
    if (base_type != nullptr) {
      emit_properties_for_type(*base_type);
    }
  }

  if (indexer_make_seq != nullptr) {
    indent(out, 4) << "int IReadOnlyCollection<" << indexer_type << ">.Count {\n";
    indent(out, 6) << "get {\n";
    indent(out, 8) << "return (int)NativeMethods."
                   << get_pinvoke_name(indexer_make_seq->_length_getter, indexer_length_remap)
                   << "(";
    if (indexer_length_remap->_has_this) {
      out << "NativeHandle";
    }
    out << ");\n";
    indent(out, 6) << "}\n";
    indent(out, 4) << "}\n\n";

    indent(out, 4) << indexer_type << " IReadOnlyList<" << indexer_type
                   << ">.this[int index] {\n";
    indent(out, 6) << "get {\n";

    string native_call = "NativeMethods." +
      get_pinvoke_name(indexer_make_seq->_element_getter, indexer_element_remap) + "(";
    if (indexer_element_remap->_has_this) {
      native_call += "NativeHandle, ";
    }
    size_t idx_param = indexer_element_remap->_has_this ? 1 : 0;
    if (idx_param < indexer_element_remap->_parameters.size()) {
      string idx_pinvoke = get_pinvoke_type(
        indexer_element_remap->_parameters[idx_param]._remap->get_new_type(), false);
      if (idx_pinvoke != "int") {
        native_call += "(" + idx_pinvoke + ")";
      }
    }
    native_call += "index)";
    CPPType *return_type_cpp = indexer_element_remap->_return_type->get_new_type();
    if (TypeManager::is_bool(return_type_cpp) ||
        TypeManager::is_simple(return_type_cpp) ||
        TypeManager::is_enum(return_type_cpp)) {
      if (TypeManager::is_enum(return_type_cpp) && is_enum_type(_objects, return_type_cpp)) {
        indent(out, 8) << "return (" << indexer_type << ")" << native_call << ";\n";
      } else {
        indent(out, 8) << "return " << native_call << ";\n";
      }
    } else if (TypeManager::is_char_pointer(return_type_cpp) ||
               TypeManager::is_const_char_pointer(return_type_cpp) ||
               indexer_type == "string") {
      indent(out, 8) << "IntPtr result = " << native_call << ";\n";
      indent(out, 8) << "return result == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(result);\n";
    } else if (is_csharp_native_object_type(_objects, return_type_cpp, indexer_type)) {
      indent(out, 8) << "IntPtr result = " << native_call << ";\n";
      indent(out, 8) << "return " << indexer_type
                     << ".__CreateFromNative(result, "
                     << get_native_ownership_name(indexer_element_remap, false)
                     << ");\n";
    } else {
      indent(out, 8) << "return " << native_call << ";\n";
    }

    indent(out, 6) << "}\n";
    indent(out, 4) << "}\n\n";

    indent(out, 4) << "IEnumerator<" << indexer_type << "> IEnumerable<"
                   << indexer_type << ">.GetEnumerator() {\n";
    indent(out, 6) << "int count = ((IReadOnlyCollection<" << indexer_type
                   << ">)this).Count;\n";
    indent(out, 6) << "for (int i = 0; i < count; i++) {\n";
    indent(out, 8) << "yield return ((IReadOnlyList<" << indexer_type
                   << ">)this)[i];\n";
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
  string class_name = get_class_name(object->_itype);
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
    std::vector<string> native_args;

    for (size_t i = 0; i < remap->_parameters.size(); ++i) {
      ParameterRemap *param_remap = remap->_parameters[i]._remap;
      TypeIndex param_type_index = get_parameter_type_for_remap(remap, i);
      bool param_nullable = is_parameter_nullable(remap, i);
      string param_type = (param_type_index != 0)
        ? get_csharp_signature_type(param_type_index, param_nullable)
        : get_csharp_signature_type_for_wrapper(param_remap, param_nullable);
      // param_type is already resolved by get_csharp_signature_type
      string param_name = get_csharp_parameter_name(remap, i);
      param_types.push_back(param_type);
      param_decls.push_back(param_type + " " + param_name);

      if (is_csharp_native_object_type(param_type_index, param_type) ||
          is_collection_type_index(param_type_index)) {
        native_args.push_back("NativeObject.Unwrap(" + param_name + ")");
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

    indent(out, indent_level) << "public " << class_name << "(";
    for (size_t i = 0; i < param_decls.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << param_decls[i];
    }
    out << ") : this(NativeMethods." << get_pinvoke_name(func, remap) << "(";
    for (size_t i = 0; i < native_args.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << native_args[i];
    }
    out << "), "
        << ((wrapper != nullptr) ? get_native_ownership_name(*wrapper, true)
                                 : get_native_ownership_name(remap, true))
        << ") {\n";
    indent(out, indent_level) << "}\n\n";
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
    std::vector<string> native_args;

    for (int i = 0; i < wrapper.number_of_parameters(); ++i) {
      TypeIndex param_type_index = wrapper.parameter_get_type(i);
      string param_type = get_csharp_signature_type(param_type_index, wrapper.parameter_is_nullable(i));

      string param_name = wrapper.parameter_has_name(i)
        ? make_csharp_identifier(wrapper.parameter_get_name(i))
        : string("param") + std::to_string(i);
      param_types.push_back(param_type);
      param_decls.push_back(param_type + " " + param_name);

      if (is_csharp_native_object_type(param_type_index, param_type) ||
          is_collection_type_index(param_type_index)) {
        native_args.push_back("NativeObject.Unwrap(" + param_name + ")");
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

    indent(out, indent_level) << "public " << class_name << "(";
    for (size_t i = 0; i < param_decls.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << param_decls[i];
    }
    out << ") : this(NativeMethods." << get_pinvoke_name(func->_ifunc, wrapper) << "(";
    for (size_t i = 0; i < native_args.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << native_args[i];
    }
    out << "), " << get_native_ownership_name(wrapper, true) << ") {\n";
    indent(out, indent_level) << "}\n\n";
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
        ? get_csharp_signature_type(param_type_index, param_nullable)
        : get_csharp_signature_type_for_wrapper(param_remap, param_nullable);

      string param_name = get_csharp_parameter_name(remap, i);
      param_types.push_back(param_type);
      param_decls.push_back(param_type + " " + param_name);
      param_names.push_back(param_name);

      AtomicToken stream_tok = csharp_stream_token_for_type(param_type_index);
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
      ? inherited_method_signature_kind(object->_itype, method_name, param_types, is_static,
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
      } else if (func->_ifunc.is_virtual()) {
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
      ? inherited_method_signature_kind(object->_itype, alias_name, param_types, is_static,
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
        alias_call = type_prefix + get_class_name(object->_itype) + "." + method_name;
      } else {
        alias_call = type_prefix + make_csharp_identifier(_current_module_name + string("Globals")) +
          "." + method_name;
      }
    }
    if (!is_interface && alias_name != method_name && alias_name != "Dispose" &&
        !is_blocked_pascal_alias(alias_name) &&
        (is_static || inherited_alias_kind == 0) &&
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
      string param_type = get_csharp_signature_type(param_type_index, param_nullable);

      string param_name = wrapper.parameter_has_name(i)
        ? make_csharp_identifier(wrapper.parameter_get_name(i))
        : string("param") + std::to_string(i - first_param);
      param_types.push_back(param_type);
      param_decls.push_back(param_type + " " + param_name);
      param_names.push_back(param_name);

      AtomicToken stream_tok = csharp_stream_token_for_type(param_type_index);
      if (stream_tok != AT_not_atomic) {
        string bridge_var = "__p3stream" + std::to_string(stream_bridges.size());
        stream_bridges.push_back({bridge_var, csharp_stream_bridge_factory(stream_tok),
                                  param_name, param_nullable});
        native_args.push_back(bridge_var + ".Handle");
      } else {
        native_args.push_back(marshal_managed_argument(param_type_index, param_type, param_name));
      }
    }

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
      ? inherited_method_signature_kind(object->_itype, method_name, param_types, is_static,
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
      } else if (func->_ifunc.is_virtual()) {
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
      ? inherited_method_signature_kind(object->_itype, alias_name, param_types, is_static,
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
        alias_call = type_prefix + get_class_name(object->_itype) + "." + method_name;
      } else {
        alias_call = type_prefix + make_csharp_identifier(_current_module_name + string("Globals")) +
          "." + method_name;
      }
    }
    if (alias_name != method_name && alias_name != "Dispose" &&
        !is_blocked_pascal_alias(alias_name) &&
        (is_static || inherited_alias_kind == 0) &&
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
  string globals_name = make_csharp_identifier(
    ((def != nullptr && def->module_name != nullptr) ? def->module_name : module_name) +
    string("Globals"));

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

    string upcast_name = get_upcast_pinvoke_name(itype, di);
    if (upcast_name.empty()) {
      continue;
    }

    const InterrogateType &base_type = idb->get_type(base_index);

    pinvoke_names.push_back(upcast_name);
    if (find_upcast_chain(base_type, target_type, pinvoke_names, visited)) {
      return true;
    }
    pinvoke_names.pop_back();
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
    return get_pinvoke_atomic_type(itype);
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
      // System.IO.Stream on the managed side; see StreamBridge.cs.
      return for_return ? "global::System.IO.Stream?" : "global::System.IO.Stream";
    }
    return get_pinvoke_atomic_type(itype);
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
        return get_qualified_interface_name(inner_type) + (for_return ? "?" : "");
      }
    }
    return "IntPtr";
  }
  if (itype.is_enum()) {
    return get_qualified_class_name(itype);
  }
  if (itype.is_class() || itype.is_struct()) {
    return get_qualified_interface_name(itype) + (for_return ? "?" : "");
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
      return get_qualified_interface_name(*itype) + (for_return ? "?" : "");
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
    return get_qualified_interface_name(*named_type);
  }

  return type_name;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_csharp_signature_type(TypeIndex type_index, bool for_return) const {
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
      return get_qualified_interface_name(original_type) + (for_return ? "?" : "");
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
    return get_qualified_interface_name(itype) + (for_return ? "?" : "");
  }
  if (itype.is_pointer()) {
    TypeIndex inner = unwrap_type_aliases(itype.get_wrapped_type());
    if (inner != 0) {
      const InterrogateType &inner_type = idb->get_type(inner);
      if (inner_type.is_class() || inner_type.is_struct()) {
        if (should_skip_csharp_type(inner_type)) {
          return "";
        }
        return get_qualified_interface_name(inner_type) + (for_return ? "?" : "");
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
    return get_qualified_class_name(itype);
  }
  if (itype.is_pointer()) {
    TypeIndex inner = unwrap_type_aliases(itype.get_wrapped_type());
    if (inner != 0) {
      const InterrogateType &inner_type = idb->get_type(inner);
      if (inner_type.is_class() || inner_type.is_struct()) {
        return get_qualified_class_name(inner_type);
      }
    }
  }

  return string();
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
  string name = get_class_name(itype);
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
  string name = get_interface_name(itype);
  string type_module = get_type_module_name(itype);

  if (!_current_module_name.empty() && !type_module.empty() &&
      type_module != _current_module_name) {
    return "global::" + prettify_namespace(type_module) + "." + name;
  }

  return name;
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
  std::vector<const InterrogateType *> secondary_base_types;
  get_secondary_base_types(itype, secondary_base_types);

  std::set<string> emitted_interfaces;
  string interface_list;
  for (const InterrogateType *base_type : secondary_base_types) {
    if (base_type == nullptr) {
      continue;
    }
    if (is_collection_facade_type(*base_type)) {
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
 *
 */
string InterfaceMakerCSharp::
get_collection_element_type_from_suffix(const string &suffix, bool for_signature) const {
  string clean_suffix = suffix;
  if (clean_suffix.length() > 6 && clean_suffix.substr(clean_suffix.length() - 6) == "_const") {
    clean_suffix = clean_suffix.substr(0, clean_suffix.length() - 6);
  }

  if (clean_suffix == "unsigned_char" || clean_suffix == "uchar") return "byte";
  if (clean_suffix == "signed_char" || clean_suffix == "char") return "sbyte";
  if (clean_suffix == "unsigned_short_int" || clean_suffix == "ushort") return "ushort";
  if (clean_suffix == "short_int" || clean_suffix == "short") return "short";
  if (clean_suffix == "unsigned_int" || clean_suffix == "uint") return "uint";
  if (clean_suffix == "int") return "int";
  if (clean_suffix == "float") return "float";
  if (clean_suffix == "double") return "double";
  if (clean_suffix == "string" || clean_suffix == "wstring" || clean_suffix == "string_const") return "string";

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  TypeIndex type_index = idb->lookup_type_by_name(clean_suffix);
  if (type_index != 0) {
    type_index = unwrap_type_aliases(type_index);
    if (type_index != 0) {
      const InterrogateType &itype = idb->get_type(type_index);
      return for_signature ? get_interface_name(itype) : get_class_name(itype);
    }
  }

  string class_name = make_csharp_identifier(clean_suffix);
  const InterrogateType *named_type = find_csharp_object_type(class_name);
  if (named_type != nullptr) {
    return for_signature ? get_interface_name(*named_type) : get_class_name(*named_type);
  }

  return class_name;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_collection_element_cpp_type_from_suffix(const string &suffix) const {
  string clean_suffix = suffix;
  if (clean_suffix.length() > 6 && clean_suffix.substr(clean_suffix.length() - 6) == "_const") {
    clean_suffix = clean_suffix.substr(0, clean_suffix.length() - 6);
  }

  if (clean_suffix == "unsigned_char" || clean_suffix == "uchar") return "unsigned char";
  if (clean_suffix == "signed_char" || clean_suffix == "char") return "signed char";
  if (clean_suffix == "unsigned_short_int" || clean_suffix == "ushort") return "unsigned short int";
  if (clean_suffix == "short_int" || clean_suffix == "short") return "short int";
  if (clean_suffix == "unsigned_int" || clean_suffix == "uint") return "unsigned int";
  if (clean_suffix == "int") return "int";
  if (clean_suffix == "float") return "float";
  if (clean_suffix == "double") return "double";
  if (clean_suffix == "string" || clean_suffix == "string_const") return "std::string";
  if (clean_suffix == "wstring") return "std::wstring";

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  TypeIndex type_index = idb->lookup_type_by_name(clean_suffix);
  if (type_index != 0) {
    type_index = unwrap_type_aliases(type_index);
    if (type_index != 0) {
      const InterrogateType &itype = idb->get_type(type_index);
      if (itype.has_true_name()) {
        return itype.get_true_name();
      } else if (itype.has_scoped_name()) {
        return itype.get_scoped_name();
      }
    }
  }

  return clean_suffix;
}

/**
 *
 */
string InterfaceMakerCSharp::
get_collection_helper_name(const InterrogateType &itype, const string &op) const {
  std::ostringstream strm;
  strm << "Collection_" << get_class_name(itype) << "_";
  TypeIndex type_index = get_type_index_for_interrogate_type(itype);
  if (type_index != 0) {
    strm << type_index << "_";
  }
  strm << op;
  return strm.str();
}

/**
 *
 */
string InterfaceMakerCSharp::
get_collection_element_type(const InterrogateType &itype, bool for_signature) const {
  string suffix = get_collection_suffix(get_csharp_type_name(itype));
  return get_collection_element_type_from_suffix(suffix, for_signature);
}

/**
 * Returns 0 if no inherited member with the same signature exists, 1 if a
 * non-virtual inherited member exists, and 2 if a virtual inherited member
 * exists.  When primary_chain_only is true, only the primary C# class chain is
 * considered; otherwise, the full wrapped interface-base closure is checked.
 */
int InterfaceMakerCSharp::
inherited_method_signature_kind(const InterrogateType &itype,
                                const string &method_name,
                                const std::vector<string> &param_types,
                                bool is_static,
                                bool primary_chain_only) {
  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();
  std::set<TypeIndex> seen;
  string target_sig = build_signature_key(method_name, param_types, is_static);

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

            std::vector<string> inherited_param_types;
              size_t first_param = remap->_has_this ? 1 : 0;
              for (size_t i = first_param; i < remap->_parameters.size(); ++i) {
                inherited_param_types.push_back(
                get_csharp_signature_type_for_wrapper(remap->_parameters[i]._remap,
                                                      is_parameter_nullable(remap, i)));
              }
            if (build_signature_key(method_name, inherited_param_types, remap_static) == target_sig) {
              return method->_ifunc.is_virtual() ? 2 : 1;
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

              std::vector<string> inherited_param_types;
              int first_param = wrapper_static ? 0 : 1;
              for (int i = first_param; i < wrapper.number_of_parameters(); ++i) {
                inherited_param_types.push_back(
                  get_csharp_signature_type(wrapper.parameter_get_type(i), wrapper.parameter_is_nullable(i)));
              }
              if (build_signature_key(method_name, inherited_param_types, wrapper_static) == target_sig) {
                return method->_ifunc.is_virtual() ? 2 : 1;
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
