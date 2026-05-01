//FLAGS: -promiscuous -string -refcount
#include <string>
#include <vector>

// Exercises virtual dispatch, namespaces, and free functions with multiple
// overloads.  Intentionally narrower than panda3d's real use so the test
// remains tractable to implement without a full dtool stack.

class Shape {
public:
  Shape();
  virtual ~Shape();

  virtual float area() const;
  virtual std::string type_name() const;

  int get_id() const;
  void set_id(int id);

private:
  int _id;
};

class Circle : public Shape {
public:
  Circle(float radius);

  float get_radius() const;
  void set_radius(float r);

  virtual float area() const override;
  virtual std::string type_name() const override;

private:
  float _radius;
};

// Free functions at module scope — test that they end up in the globals
// class and are callable.
int add_numbers(int a, int b);
float add_floats(float a, float b);
