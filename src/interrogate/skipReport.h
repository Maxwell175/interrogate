/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file skipReport.h
 * @author Maxwell175
 * @date 2026-07-13
 */

#ifndef SKIPREPORT_H
#define SKIPREPORT_H

#include "dtoolbase.h"

#include <string>
#include <iostream>

/**
 * Records API that interrogate scanned but could not expose, so that a binding
 * can never disappear silently.
 *
 * Historically a method whose parameter type had no ParameterRemap was dropped
 * with a bare `return false` -- no warning, no counter (the one diagnostic that
 * would have said so was commented out).  The method still got a record in the
 * database, but with zero callable wrappers, so it simply was not there on the
 * far side.  That is how `AsyncFuture::gather_csharp` vanished: a workaround
 * was written around a binding nobody knew had gone missing.
 *
 * Every place that decides "we cannot wrap this" must call record_skipped().
 * The tool then reports a summary before exiting -- and, with -skip-report,
 * writes the full machine-readable list.
 */

/**
 * Notes one piece of unexposed API.  `kind` is a short category ("function",
 * "parameter", "type"), `scoped_name` names the C++ entity, and `reason`
 * explains what could not be mapped -- name the offending type, since that is
 * what tells someone whether the loss is expected.
 */
void record_skipped(const std::string &kind, const std::string &scoped_name,
                    const std::string &reason);

/**
 * How many distinct entries have been recorded.  Repeated reports of the same
 * (scoped_name, reason) pair collapse into one -- an overload set can hit the
 * same unmappable type many times.
 */
int get_num_skipped();

/**
 * Writes the full list, one entry per line, as `kind<TAB>name<TAB>reason`.
 */
void write_skip_report(std::ostream &out);

/**
 * Prints the summary that the build actually sees, and writes the full list to
 * `report_filename` when that is non-empty.  `tool` names the reporting binary
 * so a combined build log stays legible.
 */
void report_skipped(const std::string &tool, const std::string &report_filename);

#endif  // SKIPREPORT_H
