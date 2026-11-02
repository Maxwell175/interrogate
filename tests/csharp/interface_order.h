//FLAGS: -promiscuous -string

// The generated C# for a class with several secondary bases must list them in
// one fixed order, the order of their interface names, not in the order of
// anything in the generator's memory.  The bases are defined in one order and
// derived from in another, neither of them alphabetical.  OrderLeaf's own
// primary base OrderStem shares OrderLeaf's pointer, and must be handed it.

class OrderBase {
public:
  OrderBase();
  int get_base() const;
};

class OrderCharlie {
public:
  OrderCharlie();
  int get_charlie() const;
};

class OrderBravo {
public:
  OrderBravo();
  int get_bravo() const;
};

class OrderStem {
public:
  OrderStem();
  int get_stem() const;
};

class OrderLeaf : public OrderStem {
public:
  OrderLeaf();
  int get_leaf() const;
};

class OrderAlpha {
public:
  OrderAlpha();
  int get_alpha() const;
};

class OrderDerived : public OrderBase, public OrderCharlie, public OrderLeaf,
                     public OrderAlpha, public OrderBravo {
public:
  OrderDerived();
};
