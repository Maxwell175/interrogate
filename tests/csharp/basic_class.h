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

enum Color {
  RED = 0,
  GREEN = 1,
  BLUE = 2,
};

Color brighter_than(Color c);
