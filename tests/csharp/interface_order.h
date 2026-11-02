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

// OrderChained owns OrderXray through two levels of secondary bases: its
// secondary base OrderZulu has OrderYankee as a secondary base, which has
// OrderXray as one.  OrderChained's constructor sets OrderYankee's pointer from
// OrderZulu's and OrderXray's from OrderYankee's, so it must set them in that
// order, although by name OrderXray comes first.

class OrderChainBase {
public:
  OrderChainBase();
  int get_chain_base() const;
};

class OrderXray {
public:
  OrderXray();
  int get_xray() const;
};

class OrderYankeeRoot {
public:
  OrderYankeeRoot();
  int get_yankee_root() const;
};

class OrderYankee : public OrderYankeeRoot, public OrderXray {
public:
  OrderYankee();
  int get_yankee() const;
};

class OrderZuluRoot {
public:
  OrderZuluRoot();
  int get_zulu_root() const;
};

class OrderZulu : public OrderZuluRoot, public OrderYankee {
public:
  OrderZulu();
  int get_zulu() const;
};

class OrderChained : public OrderChainBase, public OrderZulu {
public:
  OrderChained();
};
