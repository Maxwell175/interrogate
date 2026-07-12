//FLAGS: -promiscuous -string -refcount
#include <string>

// Basic class hierarchy: constructors, virtual methods, inheritance,
// string round-trips, enums.

class Animal {
public:
  Animal(const std::string &name);
  ~Animal();

  std::string get_name() const;
  void set_name(const std::string &name);
  int get_age() const;
  void set_age(int age);
  std::string speak() const;
  bool operator==(const Animal &other) const;
  bool operator!=(const Animal &other) const;

private:
  std::string _name;
  int _age;
};

class Dog : public Animal {
public:
  Dog(const std::string &name);
  std::string speak() const;
  bool is_good_boy() const;
};

class IdentityOnly {
public:
  IdentityOnly(int id);
  int get_id() const;

private:
  int _id;
};

class NumberBox {
public:
  NumberBox(int value);
  int get_value() const;
  NumberBox make_offset(int offset) const;
  int operator+(const NumberBox &other) const;
  int operator-() const;
  bool operator<(const NumberBox &other) const;
  bool operator>(const NumberBox &other) const;

private:
  int _value;
};

class SimpleBox {
public:
  SimpleBox(int value);
  int get_value() const;
  SimpleBox make_offset(int offset) const;
  int add(SimpleBox other) const;

private:
  int _value;
};

int add_simple_boxes(SimpleBox a, SimpleBox b);

class ForcedInterfaceBox {
public:
  ForcedInterfaceBox(int value);
  int get_value() const;
  ForcedInterfaceBox make_offset(int offset) const;
  int add(ForcedInterfaceBox other) const;

private:
  int _value;
};

enum Color {
  RED = 0,
  GREEN = 1,
  BLUE = 2,
};

Color brighter_than(Color c);
