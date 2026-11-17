#include "handles.h"

// Index 0 is no button, as in a real registry.
static const char *const button_names[] = { "none", "mouse1", "enter", "shift" };
static const int num_button_names = sizeof(button_names) / sizeof(button_names[0]);

int handles_button_count() { return num_button_names; }

TypeHandle TypeHandle::from_index(int index) {
  TypeHandle handle;
  handle._index = index;
  return handle;
}

int TypeHandle::get_index() const { return _index; }

std::string TypeHandle::get_name() const {
  return "type" + std::to_string(_index);
}

ButtonHandle::ButtonHandle(int index) : _index(index) {}

ButtonHandle::ButtonHandle(const std::string &name) {
  for (int i = 0; i < num_button_names; ++i) {
    if (name == button_names[i]) {
      _index = i;
      return;
    }
  }
}

int ButtonHandle::get_index() const { return _index; }

std::string ButtonHandle::get_name() const {
  return (_index >= 0 && _index < num_button_names) ? button_names[_index] : "?";
}

bool ButtonHandle::matches(const ButtonHandle &other) const {
  return _index == other._index;
}

int ButtonHandle::compare_to(const ButtonHandle &other) const {
  return _index - other._index;
}

// "enter" is an alias of "mouse1" here, so a handle comes back by value.
ButtonHandle ButtonHandle::get_alias() const {
  return ButtonHandle(_index == 2 ? 1 : _index);
}

ButtonList::ButtonList() {}

void ButtonList::add_button(ButtonHandle button) { _buttons.push_back(button); }

bool ButtonList::add_button_ref(const ButtonHandle &button) {
  if (has_button(button)) {
    return false;
  }
  _buttons.push_back(button);
  return true;
}

bool ButtonList::has_button(const ButtonHandle &button) const {
  for (const ButtonHandle &b : _buttons) {
    if (b.matches(button)) {
      return true;
    }
  }
  return false;
}

int ButtonList::get_num_buttons() const { return (int)_buttons.size(); }

ButtonHandle ButtonList::get_button(int n) const { return _buttons[n]; }

const ButtonHandle &ButtonList::get_last_button() const { return _buttons.back(); }

void ButtonList::set_kind(TypeHandle kind) { _kind = kind; }

TypeHandle ButtonList::get_kind() const { return _kind; }

bool ButtonList::is_kind(const TypeHandle &kind) const {
  return _kind.get_index() == kind.get_index();
}

void ButtonList::get_first_button(ButtonHandle &result) const { result = _buttons.front(); }

void ButtonList::add_button_ptr(const ButtonHandle *button) { _buttons.push_back(*button); }
