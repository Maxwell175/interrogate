//FLAGS: -promiscuous -refcount -DCPPPARSER
#pragma once
#include "shared_instantiation.h"

typedef Box<int> AlphaBox;
typedef Counted<int> AlphaCounted;

class Alpha {
public:
  Alpha();
  AlphaBox get_box() const;
  AlphaCounted *make_counted() const;

private:
  AlphaBox _box;
};
