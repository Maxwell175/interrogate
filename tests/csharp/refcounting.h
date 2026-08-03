//FLAGS: -promiscuous -string -refcount
#include <string>

// Exercises reference-counted lifetime through the binding: that constructing
// and disposing a ref-counted object is balanced (no leak), that calling a
// method inherited from a ref-counted *secondary* base does not leak a
// reference per call, that an owning CastTo keeps the object alive past the
// source, and that a cast that cannot be verified fails closed.
//
// The bug these guard against: the synthesized upcast thunks used to ref()
// their result, which the C# side drops on the floor -- so every construction
// (the ctor caches an upcast-to-RefBase pointer) and every inherited-method
// call (which re-upcasts `this`) leaked one reference, and the object could
// never reach zero.

// Test-only probes, exported at module scope (emitted as static methods on the
// module's globals class).
int rc_live_gadgets();
int rc_total_gadgets_created();
void rc_reset_counts();

// A reference-counted base.  Named ReferenceCount so both interrogate passes
// recognise it: pass 1 detects ref()/unref()/get_ref_count() by signature,
// pass 2 (which works from the serialized database) detects the base by name.
// This mirrors how every real Panda type reaches ref-counting -- through a base
// class literally named ReferenceCount.
class ReferenceCount {
public:
  ReferenceCount();
  virtual ~ReferenceCount();

  void ref() const;
  // Returns true when the count reaches zero (matches unref_delete's contract).
  bool unref() const;
  int get_ref_count() const;

private:
  mutable int _ref_count;
};

// A plain primary base, so ReferenceCount is a *secondary* base of Gadget.  That
// forces the upcast thunk both when the ctor caches the ReferenceCount pointer
// and when an inherited ReferenceCount method is called on a Gadget.
class Widget {
public:
  Widget();
  virtual ~Widget();

  int get_widget_id() const;
  void set_widget_id(int id);

private:
  int _id;
};

// Reference-counted (via ReferenceCount) and multiply-inherited.
class Gadget : public Widget, public ReferenceCount {
public:
  Gadget();
  virtual ~Gadget();

  std::string describe() const;
};

// An unrelated ref-counted type used to check that a cast which cannot be
// verified returns null instead of reinterpreting a Gadget* as a Sprocket*.
class Sprocket : public ReferenceCount {
public:
  Sprocket();

  int get_sprocket_value() const;
};

// The ref-counter's delete hook the generated bindings call.  A template, so
// interrogate ignores it while the generated wrapper .cxx (which includes this
// header) compiles against it.
template<class T>
inline void unref_delete(T *ptr) {
  if (ptr->unref()) {
    delete ptr;
  }
}
