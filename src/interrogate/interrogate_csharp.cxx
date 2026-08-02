/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file interrogate_csharp.cxx
 * @date 2026-04-11
 */

#include "interrogate.h"
#include "interfaceMakerCSharp.h"
#include "interrogateBuilder.h"
#include "interrogateDatabase.h"
#include "pnotify.h"
#include "panda_getopt_long.h"
#include "preprocess_argv.h"

#include "skipReport.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

using std::cerr;
using std::string;

CPPParser parser;

Filename output_code_filename;
Filename output_data_filename;
string output_data_basename;
bool output_module_specific = false;
bool output_function_pointers = false;
bool output_function_names = true;
bool convert_strings = true;
bool manage_reference_counts = true;
bool watch_asserts = false;
bool true_wrapper_names = false;
bool build_c_wrappers = true;
bool build_python_wrappers = false;
bool build_python_obj_wrappers = false;
bool build_python_native = false;
bool build_csharp = true;

bool track_interpreter = false;
bool save_unique_names = false;
bool no_database = false;
bool generate_spam = false;
bool left_inheritance_requires_upcast = true;
bool mangle_names = true;
CPPVisibility min_vis = V_published;
string library_name;
std::vector<std::string> library_names;
string module_name;
string skip_report_filename;
Filename csharp_output_dir;
Filename csharp_output_code_filename;
string csharp_dll_name;

std::vector<string> csharp_include_headers;
std::vector<Filename> database_search_dirs;
bool csharp_database_only_pass = true;

static const char *short_options = "h";

enum CommandOptions {
  CO_ocs = 256,
  CO_ocxx,
  CO_module,
  CO_library,
  CO_dllname,
  CO_search_dir,
  CO_module_map,
  CO_module_depends,

  CO_include_header,
  CO_skip_report,
  CO_help,
};

static struct option long_options[] = {
  { "ocs", required_argument, nullptr, CO_ocs },
  { "ocxx", required_argument, nullptr, CO_ocxx },
  { "module", required_argument, nullptr, CO_module },
  { "library", required_argument, nullptr, CO_library },
  { "dllname", required_argument, nullptr, CO_dllname },
  { "search-dir", required_argument, nullptr, CO_search_dir },
  { "module-map", required_argument, nullptr, CO_module_map },
  { "module-depends", required_argument, nullptr, CO_module_depends },

  { "include-header", required_argument, nullptr, CO_include_header },
  { "skip-report", required_argument, nullptr, CO_skip_report },
  { "help", no_argument, nullptr, CO_help },
  { nullptr }
};

static void
show_usage() {
  cerr
    << "\nUsage:\n"
    << "  interrogate_csharp --module mod.name [opts] libname.in [libname.in ...]\n"
    << "  interrogate_csharp --help\n\n"
    << "Options:\n"
    << "  --ocs DIR        Output directory for generated .cs files\n"
    << "  --ocxx FILE      Output supplemental native C++ file\n"
    << "  --module NAME    Module / namespace root (required)\n"
    << "  --library NAME   Logical library name for output grouping\n"
    << "  --dllname NAME   Native library name used by LibraryImport\n"
    << "  --module-map LIB=MOD    Map a library to its module (repeatable)\n"
    << "  --module-depends MOD=DEP[,DEP...]  Modules MOD directly depends on (repeatable)\n"
    << "  --include-header PATH  Header to include in supplemental native output\n";
}

int
main(int argc, char *argv[]) {
  extern char *optarg;
  extern int optind;
  int flag;

  preprocess_argv(argc, argv);
  flag = getopt_long_only(argc, argv, short_options, long_options, nullptr);
  while (flag != EOF) {
    switch (flag) {
    case CO_skip_report:
      skip_report_filename = optarg;
      break;

    case CO_ocs:
      csharp_output_dir = Filename::from_os_specific(optarg);
      csharp_output_dir.make_absolute();
      break;

    case CO_ocxx:
      csharp_output_code_filename = Filename::from_os_specific(optarg);
      csharp_output_code_filename.make_absolute();
      break;

    case CO_module:
      module_name = optarg;
      break;

    case CO_library:
      library_name = optarg;
      library_names.push_back(optarg);
      break;

    case CO_dllname:
      csharp_dll_name = optarg;
      break;

    case CO_search_dir:
      {
        Filename dir = Filename::from_os_specific(optarg);
        dir.make_absolute();
        database_search_dirs.push_back(dir);
      }
      break;

    case CO_module_map:
      {
        string arg = optarg;
        size_t eq = arg.find('=');
        if (eq != string::npos) {
          csharp_library_to_module[arg.substr(0, eq)] = arg.substr(eq + 1);
        }
      }
      break;

    case CO_module_depends:
      {
        // "module=dep1,dep2,..." -- the modules `module` directly depends on.
        // An entry with an empty dependency list still registers `module` as a
        // known root, giving it rank 0.
        string arg = optarg;
        size_t eq = arg.find('=');
        if (eq != string::npos) {
          string module = arg.substr(0, eq);
          csharp_module_deps.emplace(module, std::set<string>());
          string deps = arg.substr(eq + 1);
          size_t start = 0;
          while (start < deps.size()) {
            size_t comma = deps.find(',', start);
            if (comma == string::npos) {
              comma = deps.size();
            }
            string dep = deps.substr(start, comma - start);
            if (!dep.empty()) {
              csharp_module_deps[module].insert(dep);
            }
            start = comma + 1;
          }
        }
      }
      break;

    case CO_include_header:
      csharp_include_headers.push_back(optarg);
      break;

    case CO_help:
    case 'h':
      show_usage();
      return 0;

    default:
      return 1;
    }
    flag = getopt_long_only(argc, argv, short_options, long_options, nullptr);
  }

  argc -= (optind - 1);
  argv += (optind - 1);

  if (module_name.empty() || argc < 2) {
    show_usage();
    return 1;
  }

  if (csharp_dll_name.empty()) {
    csharp_dll_name = library_name.empty() ? module_name : library_name;
  }
  if (library_name.empty()) {
    library_name = module_name;
  }

  InterrogateDatabase *idb = InterrogateDatabase::get_ptr();

  // Determine which .in files belong to this module (by --library flags)
  // vs which are cross-module databases.  Record type indices from this
  // module's databases so we can filter later.
  //
  // IMPORTANT: read_file() internally uses merge_from() which deduplicates
  // types by true_name into existing TypeIndex slots.  Range-based tracking
  // (types_before/types_after) misses remapped types because merge_from does
  // not call add_type (no _all_types push) for remapped entries.  Instead we
  // do a SECOND PASS over all IDB types after ALL command-line files are
  // loaded, checking the post-merge state of each type.
  std::set<std::string> owned_libraries(library_names.begin(), library_names.end());

  for (int i = 1; i < argc; ++i) {
    Filename pathname = Filename::from_os_specific(argv[i]);
    pathname.make_absolute();

    if (output_data_filename.empty()) {
      output_data_filename = pathname;
    }

    int types_before = idb->get_num_all_types();

    if (!idb->read_file(pathname.get_fullpath())) {
      nout << "Error reading interrogate data.\n";
      return 1;
    }

    int types_after = idb->get_num_all_types();

    // Determine the correct module name for types added by this .in file.
    // Only the range [types_before, types_after) captures truly NEW type slots;
    // types remapped via merge_from retain their original TypeIndex (and will be
    // handled by the second pass below).
    string basename = pathname.get_basename_wo_extension();
    string correct_module;
    auto map_it = csharp_library_to_module.find(basename);
    if (map_it != csharp_library_to_module.end()) {
      correct_module = map_it->second;
    }

    for (int t = types_before; t < types_after; ++t) {
      TypeIndex idx = idb->get_all_type(t);
      if (!correct_module.empty()) {
        csharp_type_module_map[idx] = correct_module;
      } else {
        const InterrogateType &itype = idb->get_type(idx);
        if (itype.has_module_name()) {
          csharp_type_module_map[idx] = itype.get_module_name();
        }
      }
    }
  }

  // Second pass: scan every type in the IDB and determine ownership based on
  // the FINAL post-merge state.  A type is owned by this module if:
  //   1. It is global (F_global, 0x1) — set ONLY on the one .in file that
  //      explicitly publishes this type (the canonical authoritative source).
  //      Cross-module .in files that include the header may carry a full
  //      definition but with F_global=0, indicating they are not the owner.
  //      NOTE: fully-defined is NOT required — forward-declared types forced
  //      via "forcetype" in a .N file have F_global set but F_fully_defined
  //      cleared by define_extension_type (e.g. dxGeom, dxBody from ODE).
  //   2. Its library_name (from _def, set by the winning merge) is in
  //      owned_libraries.
  // This correctly handles types merged into pre-existing stub TypeIndex
  // values (which the range-tracking above misses) and prevents cross-module
  // copies (F_global=0) from claiming ownership of foreign types.
  {
    std::set<TypeIndex> owned_type_indices;
    int n = idb->get_num_all_types();
    for (int t = 0; t < n; ++t) {
      TypeIndex idx = idb->get_all_type(t);
      const InterrogateType &itype = idb->get_type(idx);
      if (!itype.is_global() && !itype.is_nested()) {
        continue;  // cross-module copy — not the canonical publication
        // Note: nested types (F_nested) don't have F_global set but are
        // still uniquely owned by their containing class's library.
      }
      if (itype.has_library_name()) {
        const char *lib = itype.get_library_name();
        if (lib && *lib && owned_libraries.count(string(lib)) > 0) {
          owned_type_indices.insert(idx);
          // Also update the module map for types that won a merge but weren't
          // in the per-file range (their TypeIndex was in a previous range slot)
          if (csharp_type_module_map.count(idx) == 0) {
            auto it = csharp_library_to_module.find(string(lib));
            if (it != csharp_library_to_module.end()) {
              csharp_type_module_map[idx] = it->second;
            }
          }
        }
      }
    }
    csharp_owned_type_indices = std::move(owned_type_indices);
  }

  InterrogateModuleDef def;
  std::memset(&def, 0, sizeof(def));
  def.library_name = library_name.c_str();
  // Pass 1 hashes library_name via hash_string(name, 5) to produce the
  // short prefix baked into generated symbols (`_inC<hash><remap_hash>`
  // for C wrappers, `Collection_<hash>_...` for collection helpers).
  // Pass 2 needs the same hash so its pinvokes match the pass-1
  // exports — note that collection-helper naming reads the hash from
  // the owning type's library_name, but other callers still rely on
  // _def->library_hash_name.  Storage is static to outlive def.
  static std::string library_hash_storage =
    InterrogateBuilder::hash_string(library_name, 5);
  def.library_hash_name = library_hash_storage.c_str();
  def.module_name = module_name.c_str();

  InterfaceMakerCSharp maker(&def);
  maker.generate_wrappers();

  if (!csharp_output_code_filename.empty()) {
    Filename output_dir = csharp_output_code_filename.get_dirname();
    if (!output_dir.empty()) {
      output_dir.make_dir();
    }

    std::ofstream cxx_out;
    csharp_output_code_filename.set_text();
    csharp_output_code_filename.open_write(cxx_out);
    if (cxx_out.fail()) {
      nout << "Unable to write to " << csharp_output_code_filename << "\n";
      return 1;
    }

    for (const string &header : csharp_include_headers) {
      cxx_out << "#include \"" << header << "\"\n";
    }
    if (!csharp_include_headers.empty()) {
      cxx_out << "\n";
    }

    maker.write_functions(cxx_out);
  }

  if (!csharp_output_dir.empty()) {
    std::ostringstream sink;
    maker.write_module_support(sink, nullptr, &def);
  }

  if (idb->get_error_flag()) {
    nout << "Error reading interrogate data.\n";
    return 1;
  }

  // Report what the C# mapping could not express.  These are declarations that
  // survived pass 1 -- they have callable wrappers in the database -- but have
  // no C# signature, so they are absent from the generated bindings.
  report_skipped("interrogate_csharp", skip_report_filename);

  return 0;
}
