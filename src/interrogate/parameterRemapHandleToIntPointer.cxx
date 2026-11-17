/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file parameterRemapHandleToIntPointer.cxx
 * @author Maxwell175
 * @date 2026-11-09
 */

#include "parameterRemapHandleToIntPointer.h"
#include "interfaceMaker.h"
#include "interrogate.h"
#include "typeManager.h"

#include "cppType.h"

/**
 * orig_type is a pointer or a non-const reference to a handle.
 */
ParameterRemapHandleToIntPointer::
ParameterRemapHandleToIntPointer(CPPType *orig_type) :
  ParameterRemap(orig_type),
  _is_pointer(TypeManager::is_pointer(orig_type))
{
  CPPType *pointee = _is_pointer
    ? TypeManager::unwrap_pointer(orig_type)
    : TypeManager::unwrap_reference(orig_type);
  _handle_type = TypeManager::unwrap_const(pointee);
  _new_type = TypeManager::wrap_pointer(TypeManager::get_int_type());

  // The index comes back changed or not, so the caller reads it after the call.
  set_out_parameter(true);
}

/**
 * Declares the local handle, built from the index the variable points to.  A
 * pointer may be null, and the local is then never passed (index 0 stands in).
 */
void ParameterRemapHandleToIntPointer::
prepare_parameter(std::ostream &out, int indent_level, const std::string &variable_name) {
  std::string name = _handle_type->get_local_name(&parser);
  std::string index = _is_pointer
    ? variable_name + " != nullptr ? *" + variable_name + " : 0"
    : "*" + variable_name;
  InterfaceMaker::indent(out, indent_level) << name << " " << variable_name << "_handle = ";
  if (name == "TypeHandle") {
    out << "TypeHandle::from_index(" << index << ");\n";
  } else {
    out << name << "(" << index << ");\n";
  }
}

/**
 * Passes the local handle in place of the variable: for a pointer, a pointer
 * to it, or a null pointer where the variable is one.
 */
void ParameterRemapHandleToIntPointer::
pass_parameter(std::ostream &out, const std::string &variable_name) {
  if (_is_pointer) {
    out << "(" << variable_name << " != nullptr ? &" << variable_name << "_handle : nullptr)";
  } else {
    out << variable_name << "_handle";
  }
}

/**
 * Writes the local handle's index, which the call may have changed, back to
 * the variable, unless that is a null pointer.
 */
void ParameterRemapHandleToIntPointer::
finish_parameter(std::ostream &out, int indent_level, const std::string &variable_name) {
  if (_is_pointer) {
    InterfaceMaker::indent(out, indent_level) << "if (" << variable_name << " != nullptr) {\n";
    InterfaceMaker::indent(out, indent_level + 2)
      << "*" << variable_name << " = " << variable_name << "_handle.get_index();\n";
    InterfaceMaker::indent(out, indent_level) << "}\n";
  } else {
    InterfaceMaker::indent(out, indent_level)
      << "*" << variable_name << " = " << variable_name << "_handle.get_index();\n";
  }
}

/**
 * Recorded as what the C++ takes: the handle's own type for a reference, as a
 * const reference to one is (see ParameterRemapHandleToInt::get_recorded_type),
 * and a pointer to it for a pointer, which C# may pass as null.  Either is
 * flagged as an out-parameter, so that the C# signature passes the handle by
 * ref.
 */
CPPType *ParameterRemapHandleToIntPointer::
get_recorded_type() {
  return _is_pointer ? TypeManager::wrap_pointer(_handle_type) : _handle_type;
}
