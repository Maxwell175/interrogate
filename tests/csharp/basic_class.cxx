#include "basic_class.h"

Animal::Animal(const std::string &name) : _name(name), _age(0) {}
Animal::~Animal() {}

std::string Animal::get_name() const { return _name; }
void Animal::set_name(const std::string &name) { _name = name; }
int Animal::get_age() const { return _age; }
void Animal::set_age(int age) { _age = age; }
std::string Animal::speak() const { return _name + " makes a sound"; }

Dog::Dog(const std::string &name) : Animal(name) {}
std::string Dog::speak() const { return get_name() + " barks"; }
bool Dog::is_good_boy() const { return true; }

Color brighter_than(Color c) {
  switch (c) {
  case RED:   return GREEN;
  case GREEN: return BLUE;
  case BLUE:  return RED;
  }
  return RED;
}
