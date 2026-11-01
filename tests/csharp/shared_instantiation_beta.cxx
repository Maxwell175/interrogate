#include "shared_instantiation_beta.h"

Beta::Beta() { _box.set_value(2); }
BetaBox Beta::get_box() const { return _box; }
BetaCounted *Beta::make_counted() const {
  BetaCounted *counted = new BetaCounted;
  counted->set_value(2);
  return counted;
}
