#include "handles.h"

// Index 0 is no button, as in a real registry.
static const char *const button_names[] = { "none", "mouse1", "enter", "shift" };
static const int num_button_names = sizeof(button_names) / sizeof(button_names[0]);

static int equality_calls = 0;

int handles_button_count() { return num_button_names; }

int handles_equality_calls() { return equality_calls; }

TypeHandle TypeHandle::from_index(int index) {
  TypeHandle handle;
  handle._index = index;
  return handle;
}

int TypeHandle::get_index() const { return _index; }

std::string TypeHandle::get_name() const {
  return "type" + std::to_string(_index);
}

bool TypeHandle::operator == (const TypeHandle &other) const {
  ++equality_calls;
  return _index == other._index;
}

bool TypeHandle::operator != (const TypeHandle &other) const {
  ++equality_calls;
  return _index != other._index;
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

bool ButtonHandle::operator == (const ButtonHandle &other) const {
  ++equality_calls;
  return _index == other._index;
}

bool ButtonHandle::operator != (const ButtonHandle &other) const {
  ++equality_calls;
  return _index != other._index;
}

bool ButtonHandle::operator < (const ButtonHandle &other) const {
  return _index < other._index;
}

ButtonList::ButtonList() {}

ButtonList::ButtonList(const ButtonHandle &first) { _buttons.push_back(first); }

ButtonList::ButtonList(ButtonHandle &next, int count) {
  for (int i = 0; i < count; ++i) {
    _buttons.push_back(next);
    next = ButtonHandle(next.get_index() + 1);
  }
}

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

// Reads the button it is given and moves it to the one after it in the list;
// false, and the button left alone, when there is none after it.
bool ButtonList::next_button(ButtonHandle &button) const {
  for (size_t i = 0; i + 1 < _buttons.size(); ++i) {
    if (_buttons[i].matches(button)) {
      button = _buttons[i + 1];
      return true;
    }
  }
  return false;
}

void ButtonList::clear_button(ButtonHandle *button) const { *button = ButtonHandle(0); }

void ButtonList::add_button_ptr(const ButtonHandle *button) { _buttons.push_back(*button); }

void ButtonList::next_kind(TypeHandle &kind) const {
  kind = TypeHandle::from_index(kind.get_index() + 1);
}

ButtonList ButtonList::operator + (ButtonHandle &button) const {
  ButtonList result(*this);
  result._buttons.push_back(button);
  return result;
}
