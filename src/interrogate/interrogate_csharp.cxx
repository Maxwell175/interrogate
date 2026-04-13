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
#include "interrogateDatabase.h"
#include "pnotify.h"
#include "panda_getopt_long.h"
#include "preprocess_argv.h"

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
string module_name;
Filename csharp_output_dir;
Filename csharp_output_code_filename;
string csharp_dll_name;

std::vector<string> csharp_include_headers;
bool csharp_database_only_pass = true;

static const char *short_options = "h";

enum CommandOptions {
  CO_ocs = 256,
  CO_ocxx,
  CO_module,
  CO_library,
  CO_dllname,

  CO_include_header,
  CO_help,
};

static struct option long_options[] = {
  { "ocs", required_argument, nullptr, CO_ocs },
  { "ocxx", required_argument, nullptr, CO_ocxx },
  { "module", required_argument, nullptr, CO_module },
  { "library", required_argument, nullptr, CO_library },
  { "dllname", required_argument, nullptr, CO_dllname },

  { "include-header", required_argument, nullptr, CO_include_header },
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
      break;

    case CO_dllname:
      csharp_dll_name = optarg;
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
  for (int i = 1; i < argc; ++i) {
    Filename pathname = Filename::from_os_specific(argv[i]);
    pathname.make_absolute();

    if (!idb->read_file(pathname.get_fullpath())) {
      nout << "Error reading interrogate data.\n";
      return 1;
    }
  }

  InterrogateModuleDef def;
  std::memset(&def, 0, sizeof(def));
  def.library_name = library_name.c_str();
  def.library_hash_name = library_name.c_str();
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

  return 0;
}
