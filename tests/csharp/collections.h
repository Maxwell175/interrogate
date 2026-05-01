//FLAGS: -promiscuous -string -refcount
#include <string>
#include <vector>

// interrogate detects collection facades via inheritance: any type whose
// ancestry (real inheritance + smart-pointer-holder unwrap) reaches
// std::vector<T> is exposed as an IList<T> on the managed side.
//
// This test exercises several shapes:
//   * typedef std::vector<T> foo;          (direct typedef)
//   * class Bar : public std::vector<T>;   (single-level subclass)
//   * PointerToArray-style smart pointer:  (multi-level, through a
//     smart-pointer-holder class that needs DF_pointer_to unwrapping)

typedef std::vector<int> vector_int;
typedef std::vector<std::string> vector_string;

// Models panda3d's PointerToArray pattern.  PointerToBase<T> is a smart
// pointer; interrogate detects it via its simple name and synthesizes a
// DF_pointer_to derivation from PointerToBase<T> to T in the DB.
//
// A subclass like FancyArray<T> : public PointerToBase<BackedVec<T>>
// eventually reaches std::vector<T> via:
//   FancyArray<T> → PointerToBase<BackedVec<T>>
//                 → (DF_pointer_to) → BackedVec<T>
//                 → std::vector<T>   (real inheritance)
template<class T>
class PointerToBase {
public:
  PointerToBase() : _p(nullptr) {}
  T *p() const { return _p; }
protected:
  T *_p;
};

template<class T>
class BackedVec : public std::vector<T> {
public:
  BackedVec() {}
};

template<class T>
class FancyArray : public PointerToBase<BackedVec<T>> {
public:
  typedef T value_type;
  FancyArray() { this->_p = new BackedVec<T>(); }
  int size() const { return (int)this->_p->size(); }
  T &operator[](int i) { return (*this->_p)[i]; }
  const T &operator[](int i) const { return (*this->_p)[i]; }
  void push_back(T val) { this->_p->push_back(val); }
  void clear() { this->_p->clear(); }
  void resize(int n) { this->_p->resize(n); }
};

// Instantiate with a concrete type so interrogate emits code for it.
typedef FancyArray<int> fancy_array_int;

class Bag {
public:
  Bag();

  // Blittable primitive vector — AsSpan path with zero-copy.
  void add_int(int x);
  vector_int get_ints() const;
  void set_ints(vector_int ints);
  int int_count() const;
  int total() const;

  // Non-blittable (string) vector — the one that hit the CoTaskMemFree bug.
  void add_name(const std::string &n);
  vector_string get_names() const;
  void set_names(vector_string names);
  int name_count() const;
  std::string joined_names() const;

private:
  vector_int _ints;
  vector_string _names;
};
