//FLAGS: -promiscuous -refcount -DCPPPARSER
#pragma once
#include "shared_instantiation.h"

typedef Box<int> BetaBox;
typedef Counted<int> BetaCounted;

class Beta {
public:
  Beta();
  BetaBox get_box() const;
  BetaCounted *make_counted() const;

private:
  BetaBox _box;
};
