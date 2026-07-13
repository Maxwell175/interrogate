/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file skipReport.cxx
 * @author Maxwell175
 * @date 2026-07-13
 */

#include "skipReport.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <vector>

using std::string;

namespace {

struct SkipEntry {
  string _kind;
  string _name;
  string _reason;

  bool operator < (const SkipEntry &other) const {
    if (_name != other._name) {
      return _name < other._name;
    }
    if (_reason != other._reason) {
      return _reason < other._reason;
    }
    return _kind < other._kind;
  }
};

// A set, not a vector: one unmappable type in an overload set is reported once
// per overload, and the same type recurs across many classes.  We want the
// distinct losses, in a stable order.
std::set<SkipEntry> &
get_entries() {
  static std::set<SkipEntry> entries;
  return entries;
}

// How many entries to print inline before deferring to the report file.  The
// point of the summary is to be noticed, not to bury the rest of the build.
const size_t max_inline = 20;

}  // namespace

void
record_skipped(const string &kind, const string &scoped_name,
               const string &reason) {
  SkipEntry entry;
  entry._kind = kind;
  entry._name = scoped_name;
  entry._reason = reason;
  get_entries().insert(entry);
}

int
get_num_skipped() {
  return (int)get_entries().size();
}

void
write_skip_report(std::ostream &out) {
  for (const SkipEntry &entry : get_entries()) {
    out << entry._kind << '\t' << entry._name << '\t' << entry._reason << '\n';
  }
}

void
report_skipped(const string &tool, const string &report_filename) {
  const std::set<SkipEntry> &entries = get_entries();

  if (entries.empty()) {
    // Truncate any previous report: a stale file left behind after the last drop
    // was fixed reads exactly like a drop that is still there.
    if (!report_filename.empty()) {
      std::ofstream report(report_filename.c_str(), std::ios::out | std::ios::trunc);
    }
    return;
  }

  if (!report_filename.empty()) {
    std::ofstream report(report_filename.c_str(), std::ios::out | std::ios::trunc);
    if (report) {
      write_skip_report(report);
    } else {
      std::cerr << tool << ": unable to write skip report to "
                << report_filename << "\n";
    }
  }

  std::cerr << tool << ": " << entries.size()
            << " C++ declaration(s) could not be exposed:\n";

  size_t shown = 0;
  for (const SkipEntry &entry : entries) {
    if (shown >= max_inline) {
      std::cerr << tool << ":   ... and " << (entries.size() - shown)
                << " more\n";
      break;
    }
    std::cerr << tool << ":   " << entry._name << " -- " << entry._reason
              << "\n";
    ++shown;
  }

  if (report_filename.empty()) {
    std::cerr << tool << ": pass -skip-report <file> for the full list.\n";
  } else {
    std::cerr << tool << ": full list written to " << report_filename << "\n";
  }
}
