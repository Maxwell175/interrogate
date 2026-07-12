#include "basic_class.h"

Animal::Animal(const std::string &name) : _name(name), _age(0) {}
Animal::~Animal() {}

std::string Animal::get_name() const { return _name; }
void Animal::set_name(const std::string &name) { _name = name; }
int Animal::get_age() const { return _age; }
void Animal::set_age(int age) { _age = age; }
std::string Animal::speak() const { return _name + " makes a sound"; }
bool Animal::operator==(const Animal &other) const { return _name == other._name && _age == other._age; }
bool Animal::operator!=(const Animal &other) const { return !(*this == other); }

Dog::Dog(const std::string &name) : Animal(name) {}
std::string Dog::speak() const { return get_name() + " barks"; }
bool Dog::is_good_boy() const { return true; }

IdentityOnly::IdentityOnly(int id) : _id(id) {}
int IdentityOnly::get_id() const { return _id; }

NumberBox::NumberBox(int value) : _value(value) {}
int NumberBox::get_value() const { return _value; }
NumberBox NumberBox::make_offset(int offset) const { return NumberBox(_value + offset); }
int NumberBox::operator+(const NumberBox &other) const { return _value + other._value; }
int NumberBox::operator-() const { return -_value; }
bool NumberBox::operator<(const NumberBox &other) const { return _value < other._value; }
bool NumberBox::operator>(const NumberBox &other) const { return _value > other._value; }

SimpleBox::SimpleBox(int value) : _value(value) {}
int SimpleBox::get_value() const { return _value; }
SimpleBox SimpleBox::make_offset(int offset) const { return SimpleBox(_value + offset); }
int SimpleBox::add(SimpleBox other) const { return _value + other._value; }

int add_simple_boxes(SimpleBox a, SimpleBox b) {
  return a.get_value() + b.get_value();
}

ForcedInterfaceBox::ForcedInterfaceBox(int value) : _value(value) {}
int ForcedInterfaceBox::get_value() const { return _value; }
ForcedInterfaceBox ForcedInterfaceBox::make_offset(int offset) const { return ForcedInterfaceBox(_value + offset); }
int ForcedInterfaceBox::add(ForcedInterfaceBox other) const { return _value + other._value; }

Color brighter_than(Color c) {
  switch (c) {
  case RED:   return GREEN;
  case GREEN: return BLUE;
  case BLUE:  return RED;
  }
  return RED;
}
