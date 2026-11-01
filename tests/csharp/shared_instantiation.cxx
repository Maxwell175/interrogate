// Links the supplementals of two libraries that both publish Box<int> and
// Counted<int> into one executable, the way a NativeAOT executable links every
// module's archive, and calls Box<int>'s delete helper, Counted<int>'s ref and
// unref-delete helpers, and each library's own class's delete helper, under the
// names the two libraries' C# bindings declare.
// shared_instantiation_entry_points.h is written by the test from those
// declarations (RunSharedInstantiationTest.cmake).
#include "shared_instantiation_alpha.h"
#include "shared_instantiation_beta.h"
#include "shared_instantiation_entry_points.h"

extern "C" void ALPHA_BOX_DESTR(Box<int> *self);
extern "C" void BETA_BOX_DESTR(Box<int> *self);
extern "C" void ALPHA_COUNTED_REF(Counted<int> *self);
extern "C" void BETA_COUNTED_REF(Counted<int> *self);
extern "C" void ALPHA_COUNTED_UNREF(Counted<int> *self);
extern "C" void BETA_COUNTED_UNREF(Counted<int> *self);
extern "C" void ALPHA_DESTR(Alpha *self);
extern "C" void BETA_DESTR(Beta *self);

namespace {
  // Takes two references through one library's ref helper and lets both go
  // through its unref-delete helper; true if the counts were what each call
  // should leave.
  bool counts_through(Counted<int> *counted,
                      void (*ref)(Counted<int> *), void (*unref)(Counted<int> *)) {
    ref(counted);
    ref(counted);
    bool held = counted->get_ref_count() == 2;
    unref(counted);
    bool one = counted->get_ref_count() == 1;
    unref(counted);   // reaches zero: deleted
    return held && one;
  }
}

int main() {
  Alpha *alpha = new Alpha;
  Beta *beta = new Beta;
  Box<int> *alpha_box = new Box<int>(alpha->get_box());
  Box<int> *beta_box = new Box<int>(beta->get_box());
  int value = alpha_box->get_value() * 10 + beta_box->get_value();
  ALPHA_BOX_DESTR(alpha_box);
  BETA_BOX_DESTR(beta_box);

  Counted<int> *alpha_counted = alpha->make_counted();
  Counted<int> *beta_counted = beta->make_counted();
  value += 100 * alpha_counted->get_value() + 1000 * beta_counted->get_value();
  bool counted = counts_through(alpha_counted, &ALPHA_COUNTED_REF, &ALPHA_COUNTED_UNREF) &&
                 counts_through(beta_counted, &BETA_COUNTED_REF, &BETA_COUNTED_UNREF);

  ALPHA_DESTR(alpha);
  BETA_DESTR(beta);
  return value == 2112 && counted ? 0 : 1;
}
