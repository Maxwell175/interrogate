//FLAGS: -promiscuous
// Regression coverage for the "keep managed arguments alive across the native
// call" fix.  A native-object argument -- and an instance member's `this` -- is
// handed to native code as a raw handle, and that handle is the wrapper's last
// managed use.  Without a GC.KeepAlive after the call the JIT may let the
// wrapper be finalized (freeing / unref'ing the native object) while C++ is
// still dereferencing its pointer.
//
// This exact shape crashed in the field once the upcast-thunk leak fix let
// temporaries actually be collected: TexturePool.LoadTexture(
//   Filename.FromOsSpecific(path)) freed the temporary Filename mid-call, and
// Texture::set_filename then assigned from a freed std::string.
//
// The driver both (a) exercises the generated bindings for correctness and
// (b) asserts the generated C# actually contains the GC.KeepAlive calls -- a
// pure runtime race would only fail probabilistically, and only in optimized
// code, so the deterministic guard is the source assertion.

// Test-only probes exported at module scope (emitted as globals-class statics).
int kp_live_probes();
void kp_reset_counts();

// A plain heap object with a destructor.  A freshly-`new`'d C# wrapper for one
// of these is Owned, so its finalizer runs the destructor -- the thing that
// would free it out from under an in-flight native call.
class Probe {
public:
  Probe(long id);
  ~Probe();

  long id() const;

private:
  long _id;
};

// A native-object argument to a *static* method: the reported bug's exact shape.
// The generated ReadProbe(Probe) must GC.KeepAlive(probe) after the native call.
long read_probe(Probe *p);

// Holds a (borrowed) Probe.  Its constructor takes a native-object argument, so
// it is routed through the private __p3Create* helper that keeps the argument
// alive; set_probe is an instance method taking a native-object argument (keeps
// both `this` and the value alive); holder_probe_id is an instance method that
// reads through `this` (keeps `this` alive).
class Holder {
public:
  Holder();
  Holder(Probe *p);
  ~Holder();

  void set_probe(Probe *p);
  long holder_probe_id() const;

private:
  Probe *_probe;
};
