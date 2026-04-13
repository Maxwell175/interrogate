//FLAGS: -promiscuous -string -refcount
#include <string>

class Animal {
public:
  Animal(const std::string &name);
  ~Animal();

  std::string get_name() const;
  void set_name(const std::string &name);
  int get_age() const;
  void set_age(int age);
  void speak();
};

class Dog : public Animal {
public:
  Dog(const std::string &name);
  void fetch();
  bool is_good_boy() const;
};

enum Color {
  RED = 0,
  GREEN = 1,
  BLUE = 2,
};
