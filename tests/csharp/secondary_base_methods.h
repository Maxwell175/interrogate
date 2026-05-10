//FLAGS: -promiscuous -string -refcount
#include <string>

// Exercises multiple inheritance: the managed binding should expose
// methods from BOTH primary and secondary bases on the derived type, since
// C# has no multiple inheritance.

class PrimaryBase {
public:
  PrimaryBase();
  virtual ~PrimaryBase();

  int get_id() const;
  void set_id(int id);

private:
  int _id;
};

class TextLike {
public:
  TextLike();
  virtual ~TextLike();

  void set_text(const std::string &text);
  std::string get_text() const;

private:
  std::string _text;
};

// Derived from two bases — the managed DerivedTextNode class should have
// both get_id/set_id (from PrimaryBase) and get_text/set_text (from TextLike).
class DerivedTextNode : public PrimaryBase, public TextLike {
public:
  DerivedTextNode();
};

// Test passing a secondary-base pointer as a parameter.  This exercises
// the multiple-inheritance offset adjustment bug: when attach_child takes
// a PrimaryBase*, but the caller passes a DerivedTextNode*, the C wrapper
// must apply the offset to reach the PrimaryBase subobject.
class Container {
public:
  Container();

  // Attach a child — takes PrimaryBase* (the secondary base of DerivedTextNode).
  void attach_child(PrimaryBase *child);

  // Query last attached child's ID (for test verification).
  int get_last_child_id() const;

private:
  PrimaryBase *_last_child;
};

// Deep chain test: Create a 4-level inheritance chain to test recursive upcast finding
// Level1: Primary, BaseA
// Level2: Level1, BaseB
// Level3: Level2, BaseC
// Level4: Level3, BaseD
// The deepest chain is Level4 -> Level3 -> Level2 -> Level1 -> BaseA (4 hops!)

class BaseA {
public:
  BaseA();
  void set_a(int val);
  int get_a() const;
private:
  int _a;
};

class BaseB {
public:
  BaseB();
  void set_b(int val);
  int get_b() const;
private:
  int _b;
};

class BaseC {
public:
  BaseC();
  void set_c(int val);
  int get_c() const;
private:
  int _c;
};

class BaseD {
public:
  BaseD();
  void set_d(int val);
  int get_d() const;
private:
  int _d;
};

class Level1 : public PrimaryBase, public BaseA {
public:
  Level1();
};

class Level2 : public Level1, public BaseB {
public:
  Level2();
};

class Level3 : public Level2, public BaseC {
public:
  Level3();
};

class Level4 : public Level3, public BaseD {
public:
  Level4();
};

// Test class that accepts each base
class DeepTester {
public:
  DeepTester();
  void test_base_a(BaseA *obj);
  void test_base_b(BaseB *obj);
  void test_base_c(BaseC *obj);
  void test_base_d(BaseD *obj);
  int get_last_a() const;
  int get_last_b() const;
  int get_last_c() const;
  int get_last_d() const;
private:
  int _last_a, _last_b, _last_c, _last_d;
};

// Diamond inheritance test:
//
//        DiamondBase
//        /        \
//   DiamondLeft  DiamondRight
//        \        /
//       DiamondBottom
//
// DiamondBottom has TWO paths to DiamondBase (non-virtual, so two subobjects).
// Each arm also has its own method.

class DiamondBase {
public:
  DiamondBase();
  void set_diamond_val(int val);
  int get_diamond_val() const;
private:
  int _diamond_val;
};

class DiamondLeft : public DiamondBase {
public:
  DiamondLeft();
  void set_left(int val);
  int get_left() const;
private:
  int _left;
};

class DiamondRight : public DiamondBase {
public:
  DiamondRight();
  void set_right(int val);
  int get_right() const;
private:
  int _right;
};

// Primary base is DiamondLeft, secondary base is DiamondRight.
class DiamondBottom : public DiamondLeft, public DiamondRight {
public:
  DiamondBottom();
  void set_bottom(int val);
  int get_bottom() const;
private:
  int _bottom;
};

class DiamondTester {
public:
  DiamondTester();
  void test_left(DiamondLeft *obj);
  void test_right(DiamondRight *obj);
  int get_last_left() const;
  int get_last_right() const;
private:
  int _last_left, _last_right;
};
