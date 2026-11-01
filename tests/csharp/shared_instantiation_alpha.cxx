#include "shared_instantiation_alpha.h"

Alpha::Alpha() { _box.set_value(1); }
AlphaBox Alpha::get_box() const { return _box; }
AlphaCounted *Alpha::make_counted() const {
  AlphaCounted *counted = new AlphaCounted;
  counted->set_value(1);
  return counted;
}
