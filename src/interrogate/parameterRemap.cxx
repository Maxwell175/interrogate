/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file parameterRemap.cxx
 * @author drose
 * @date 2000-08-01
 */

#include "parameterRemap.h"

using std::string;


/**
 *
 */
ParameterRemap::
~ParameterRemap() {
}

/**
 * Outputs an expression that converts the indicated variable from the
 * original type to the new type, for passing into the actual C++ function.
 */
void ParameterRemap::
pass_parameter(std::ostream &out, const string &variable_name) {
  out << variable_name;
}

/**
 * This will be called immediately before get_return_expr().  It outputs
 * whatever lines the remapper needs to the function to set up its return
 * value, e.g.  to declare a temporary variable or something.  It should
 * return the modified expression.
 */
string ParameterRemap::
prepare_return_expr(std::ostream &, int, const string &expression) {
  return expression;
}

/**
 * Returns an expression that evalutes to the appropriate value type for
 * returning from the function, given an expression of the original type.
 */
string ParameterRemap::
get_return_expr(const string &expression) {
  return expression;
}

/**
 * Returns the string that converts the expression stored in the indicated
 * temporary variable to the appropriate return value type.  This is normally
 * a pass-through, but in cases when the temporary variable type must be
 * different than the return type (i.e.  get_temporary_type() !=
 * get_new_type()), this might perform some operation.
 */
string ParameterRemap::
temporary_to_return(const string &temporary) {
  return temporary;
}

/**
 * Returns true if the return value represents a value that was newly
 * allocated, and hence must be explicitly deallocated later by the caller.
 */
bool ParameterRemap::
return_value_needs_management() {
  return false;
}

/**
 * If return_value_needs_management() returns true, this should return the
 * index of the function that should be called when it is time to destruct the
 * return value.  It will generally be the same as the destructor for the
 * class we just returned a pointer to.
 */
FunctionIndex ParameterRemap::
get_return_value_destructor() {
  return 0;
}

/**
 * This is a hack around a problem VC++ has with overly-complex expressions,
 * particularly in conjunction with the 'new' operator.  If this parameter
 * type is one that will probably give VC++ a headache, this should be set
 * true to indicate that the code generator should save the return value
 * expression into a temporary variable first, and pass the temporary variable
 * name in instead.
 */
bool ParameterRemap::
return_value_should_be_simple() {
  return false;
}


/**
 * Returns the AtomicToken that the new_type should be recorded as in the
 * InterrogateDatabase, or AT_not_atomic if the parameter's new_type is the
 * CPPType returned by get_new_type().  See ParameterRemap.h.
 */
AtomicToken ParameterRemap::
get_new_atomic_token() {
  return AT_not_atomic;
}

/**
 * Convenience wrapper retained for callers that only care about the
 * string-vs-not distinction.  Prefer get_new_atomic_token() in new code.
 */
bool ParameterRemap::
new_type_is_atomic_string() {
  return get_new_atomic_token() == AT_string;
}

/**
 * Returns true if this is the "this" parameter.
 */
bool ParameterRemap::
is_this() {
  return false;
}
