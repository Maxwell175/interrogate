/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file parameterRemapHandleToIntPointer.h
 * @author Maxwell175
 * @date 2026-11-09
 */

#ifndef PARAMETERREMAPHANDLETOINTPOINTER_H
#define PARAMETERREMAPHANDLETOINTPOINTER_H

#include "dtoolbase.h"

#include "parameterRemap.h"

/**
 * A ParameterRemap class that passes a pointer or a non-const reference to a
 * handle (see ParameterRemapHandleToInt) through a temporary, for C#: the
 * caller holds only the handle's index, so it gives a pointer to that index;
 * the wrapper builds a local handle from it, passes the function that, and
 * writes the local's index back afterwards.
 */
class ParameterRemapHandleToIntPointer : public ParameterRemap {
public:
  ParameterRemapHandleToIntPointer(CPPType *orig_type);

  virtual void prepare_parameter(std::ostream &out, int indent_level,
                                 const std::string &variable_name);
  virtual void pass_parameter(std::ostream &out, const std::string &variable_name);
  virtual void finish_parameter(std::ostream &out, int indent_level,
                                const std::string &variable_name);
  virtual CPPType *get_recorded_type();

private:
  CPPType *_handle_type;
};

#endif
