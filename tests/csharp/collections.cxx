#include "collections.h"

Roster::Roster() {}
void Roster::add(const std::string &name) { _members.push_back(name); }
int Roster::get_num_members() const { return (int)_members.size(); }
std::string Roster::get_member(int i) const { return _members[i]; }
std::string Roster::operator [](int i) const { return _members[i]; }
int Roster::size() const { return (int)_members.size(); }

Bag::Bag() {}

void Bag::add_int(int x) { _ints.push_back(x); }
vector_int Bag::get_ints() const { return _ints; }
void Bag::set_ints(vector_int ints) { _ints = std::move(ints); }
int Bag::int_count() const { return (int)_ints.size(); }
int Bag::total() const {
  int sum = 0;
  for (int v : _ints) sum += v;
  return sum;
}

void Bag::add_name(const std::string &n) { _names.push_back(n); }
vector_string Bag::get_names() const { return _names; }
void Bag::set_names(vector_string names) { _names = std::move(names); }
int Bag::name_count() const { return (int)_names.size(); }

std::string Bag::joined_names() const {
  std::string out;
  for (size_t i = 0; i < _names.size(); ++i) {
    if (i != 0) out += ",";
    out += _names[i];
  }
  return out;
}
