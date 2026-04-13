//FLAGS: -promiscuous -string -refcount
#include <string>

class Shape {
__published:
  Shape();
  virtual ~Shape();
  virtual float area() const;
  virtual std::string type_name() const;
  int get_id() const;
  __make_property(id, get_id);
};

class Circle : public Shape {
__published:
  Circle(float radius);
  float get_radius() const;
  void set_radius(float r);
  __make_property(radius, get_radius, set_radius);
  virtual float area() const;
  virtual std::string type_name() const;
};

class Serializable {
__published:
  Serializable();
  virtual ~Serializable();
  virtual std::string serialize() const;
};

class Printable {
__published:
  Printable();
  virtual ~Printable();
  virtual std::string to_string() const;
};

class Document : public Serializable, public Printable {
__published:
  Document(const std::string &title);
  virtual ~Document();
  std::string get_title() const;
  __make_property(title, get_title);
  virtual std::string serialize() const;
  virtual std::string to_string() const;
};

class Renderable {
__published:
  virtual void render() const = 0;
  virtual int vertex_count() const = 0;
  virtual ~Renderable();
};

class Mesh : public Renderable {
__published:
  Mesh(int num_verts);
  virtual void render() const;
  virtual int vertex_count() const;
};

class IntArray {
__published:
  IntArray();
  ~IntArray();
  int get_num_elements() const;
  int get_element(int n) const;
  void add_element(int val);
  __make_seq(get_elements, get_num_elements, get_element);
};

enum Priority {
  P_LOW = 0,
  P_MEDIUM = 1,
  P_HIGH = 2,
};
