// Two templates, instantiated by the public interface of two libraries
// (shared_instantiation_alpha.h and shared_instantiation_beta.h), the way
// pvector<NodePath> is published by more than one library of a project.  Both
// libraries scan this header, so each one's pass 1 sees Box<int> and
// Counted<int> as its own types and emits the per-type helpers for them: the
// delete helper for Box<int>, and the unref-delete and ref helpers for
// Counted<int>, which is reference-counted.
#pragma once

// A reference-counted base.  Named ReferenceCount so both interrogate passes
// recognise it, as in refcounting.h.  Inline, because both libraries' supplementals
// compile this header.
class ReferenceCount {
public:
  ReferenceCount() : _ref_count(0) {}
  virtual ~ReferenceCount() {}

  void ref() const { ++_ref_count; }
  // Returns true when the count reaches zero (matches unref_delete's contract).
  bool unref() const { return --_ref_count == 0; }
  int get_ref_count() const { return _ref_count; }

private:
  mutable int _ref_count;
};

template<class T>
class Box {
public:
  Box() : _value() {}
  T get_value() const { return _value; }
  void set_value(T value) { _value = value; }

private:
  T _value;
};

template<class T>
class Counted : public ReferenceCount {
public:
  Counted() : _value() {}
  T get_value() const { return _value; }
  void set_value(T value) { _value = value; }

private:
  T _value;
};

// The ref-counter's delete hook the generated unref helpers call.  A template,
// so interrogate ignores it.
template<class T>
inline void unref_delete(T *ptr) {
  if (ptr->unref()) {
    delete ptr;
  }
}
