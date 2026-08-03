#include "refcounting.h"

namespace {
  int g_live_gadgets = 0;
  int g_total_gadgets = 0;
}

int rc_live_gadgets() { return g_live_gadgets; }
int rc_total_gadgets_created() { return g_total_gadgets; }
void rc_reset_counts() { g_live_gadgets = 0; g_total_gadgets = 0; }

ReferenceCount::ReferenceCount() : _ref_count(0) {}
ReferenceCount::~ReferenceCount() {}

void ReferenceCount::ref() const { ++_ref_count; }
bool ReferenceCount::unref() const { return --_ref_count == 0; }
int ReferenceCount::get_ref_count() const { return _ref_count; }

Widget::Widget() : _id(0) {}
Widget::~Widget() {}

int Widget::get_widget_id() const { return _id; }
void Widget::set_widget_id(int id) { _id = id; }

Gadget::Gadget() {
  ++g_live_gadgets;
  ++g_total_gadgets;
}

Gadget::~Gadget() {
  --g_live_gadgets;
}

std::string Gadget::describe() const {
  return "gadget#" + std::to_string(get_widget_id());
}

Sprocket::Sprocket() {}

int Sprocket::get_sprocket_value() const { return 42; }
