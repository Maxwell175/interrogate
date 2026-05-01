/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file parameterRemapBasicStringToString.cxx
 * @author drose
 * @date 2000-08-09
 */

#include "parameterRemapBasicStringToString.h"
#include "interfaceMaker.h"
#include "interrogate.h"

using std::ostream;
using std::string;

/**
 *
 */
ParameterRemapBasicStringToString::
ParameterRemapBasicStringToString(CPPType *orig_type) :
  ParameterRemapToString(orig_type)
{
  static CPPType *const_char_star_type = nullptr;
  if (const_char_star_type == nullptr) {
    const_char_star_type = parser.parse_type("const char *");
  }

  _new_type = const_char_star_type;
}

/**
 * Outputs an expression that converts the indicated variable from the
 * original type to the new type, for passing into the actual C++ function.
 */
void ParameterRemapBasicStringToString::
pass_parameter(ostream &out, const string &variable_name) {
  out << "std::string(" << variable_name << ")";
}

/**
 * This will be called immediately before get_return_expr().  It outputs
 * whatever lines the remapper needs to the function to set up its return
 * value, e.g.  to declare a temporary variable or something.  It should
 * return the modified expression.
 */
string ParameterRemapBasicStringToString::
prepare_return_expr(ostream &out, int indent_level, const string &expression) {
  // The returned c_str() must outlive the wrapper's return statement, so
  // that the caller can copy the bytes out.  thread_local storage keeps the
  // pointer valid until the next call on the same thread, while still
  // refreshing the contents on every call.  (A plain `static` would fix the
  // first call forever; a stack-local `std::string` would free the bytes
  // before the caller can read them.)
  InterfaceMaker::indent(out, indent_level)
    << "thread_local std::string string_holder;\n";
  InterfaceMaker::indent(out, indent_level)
    << "string_holder = " << expression << ";\n";
  return "string_holder";
}

/**
 * Returns an expression that evalutes to the appropriate value type for
 * returning from the function, given an expression of the original type.
 */
string ParameterRemapBasicStringToString::
get_return_expr(const string &expression) {
  return "string_holder.c_str()";
}

/**
 *
 */
ParameterRemapBasicWStringToWString::
ParameterRemapBasicWStringToWString(CPPType *orig_type) :
  ParameterRemapToWString(orig_type)
{
  static CPPType *const_wchar_star_type = nullptr;
  if (const_wchar_star_type == nullptr) {
    const_wchar_star_type = parser.parse_type("const wchar_t *");
  }

  _new_type = const_wchar_star_type;
}

/**
 * Outputs an expression that converts the indicated variable from the
 * original type to the new type, for passing into the actual C++ function.
 */
void ParameterRemapBasicWStringToWString::
pass_parameter(ostream &out, const string &variable_name) {
  out << "std::wstring(" << variable_name << ")";
}

/**
 * This will be called immediately before get_return_expr().  It outputs
 * whatever lines the remapper needs to the function to set up its return
 * value, e.g.  to declare a temporary variable or something.  It should
 * return the modified expression.
 */
string ParameterRemapBasicWStringToWString::
prepare_return_expr(ostream &out, int indent_level, const string &expression) {
  // See ParameterRemapBasicStringToString::prepare_return_expr for the
  // thread_local rationale.
  InterfaceMaker::indent(out, indent_level)
    << "thread_local std::wstring string_holder;\n";
  InterfaceMaker::indent(out, indent_level)
    << "string_holder = " << expression << ";\n";
  return "string_holder";
}

/**
 * Returns an expression that evalutes to the appropriate value type for
 * returning from the function, given an expression of the original type.
 */
string ParameterRemapBasicWStringToWString::
get_return_expr(const string &expression) {
  return "string_holder.c_str()";
}
