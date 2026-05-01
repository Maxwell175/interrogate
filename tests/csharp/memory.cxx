#include "memory.h"

#include <atomic>
#include <ostream>
#include <istream>

namespace {
  std::atomic<int> g_live{0};
  std::atomic<int> g_total{0};
}

int memory_live_trackers() { return g_live.load(std::memory_order_relaxed); }
int memory_total_trackers_created() { return g_total.load(std::memory_order_relaxed); }
void memory_reset_counts() {
  g_live.store(0, std::memory_order_relaxed);
  g_total.store(0, std::memory_order_relaxed);
}

Tracker::Tracker(const std::string &label) : _label(label) {
  g_live.fetch_add(1, std::memory_order_relaxed);
  g_total.fetch_add(1, std::memory_order_relaxed);
}

Tracker::~Tracker() {
  g_live.fetch_sub(1, std::memory_order_relaxed);
}

std::string Tracker::get_label() const { return _label; }
std::string Tracker::echo_label() const { return _label; }

void Tracker::write_label(std::ostream &out) const {
  out << _label;
  out.flush();
}

int Tracker::read_into_label(std::istream &in) {
  _label.clear();
  char c;
  int count = 0;
  while (in.get(c)) {
    _label.push_back(c);
    ++count;
  }
  return count;
}

std::string Tracker::combined_label(Tracker *other) const {
  if (other == nullptr) {
    return _label + "+<null>";
  }
  return _label + "+" + other->_label;
}

vector_string Tracker::build_labels(int count) const {
  vector_string out;
  out.reserve(count);
  for (int i = 0; i < count; ++i) {
    out.push_back(_label + "-" + std::to_string(i));
  }
  return out;
}
