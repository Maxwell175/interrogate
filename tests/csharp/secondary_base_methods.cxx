#include "secondary_base_methods.h"

PrimaryBase::PrimaryBase() : _id(0) {}
PrimaryBase::~PrimaryBase() {}
int PrimaryBase::get_id() const { return _id; }
void PrimaryBase::set_id(int id) { _id = id; }

TextLike::TextLike() {}
TextLike::~TextLike() {}
void TextLike::set_text(const std::string &text) { _text = text; }
std::string TextLike::get_text() const { return _text; }

DerivedTextNode::DerivedTextNode() {}

Container::Container() : _last_child(nullptr) {}

void Container::attach_child(PrimaryBase *child) {
  _last_child = child;
}

int Container::get_last_child_id() const {
  return _last_child ? _last_child->get_id() : -1;
}

// Deep chain implementations
BaseA::BaseA() : _a(0) {}
void BaseA::set_a(int val) { _a = val; }
int BaseA::get_a() const { return _a; }

BaseB::BaseB() : _b(0) {}
void BaseB::set_b(int val) { _b = val; }
int BaseB::get_b() const { return _b; }

BaseC::BaseC() : _c(0) {}
void BaseC::set_c(int val) { _c = val; }
int BaseC::get_c() const { return _c; }

BaseD::BaseD() : _d(0) {}
void BaseD::set_d(int val) { _d = val; }
int BaseD::get_d() const { return _d; }

Level1::Level1() {}
Level2::Level2() {}
Level3::Level3() {}
Level4::Level4() {}

DeepTester::DeepTester() : _last_a(0), _last_b(0), _last_c(0), _last_d(0) {}
void DeepTester::test_base_a(BaseA *obj) { _last_a = obj->get_a(); }
void DeepTester::test_base_b(BaseB *obj) { _last_b = obj->get_b(); }
void DeepTester::test_base_c(BaseC *obj) { _last_c = obj->get_c(); }
void DeepTester::test_base_d(BaseD *obj) { _last_d = obj->get_d(); }
int DeepTester::get_last_a() const { return _last_a; }
int DeepTester::get_last_b() const { return _last_b; }
int DeepTester::get_last_c() const { return _last_c; }
int DeepTester::get_last_d() const { return _last_d; }

// Diamond inheritance implementations
DiamondBase::DiamondBase() : _diamond_val(0) {}
void DiamondBase::set_diamond_val(int val) { _diamond_val = val; }
int DiamondBase::get_diamond_val() const { return _diamond_val; }

DiamondLeft::DiamondLeft() : _left(0) {}
void DiamondLeft::set_left(int val) { _left = val; }
int DiamondLeft::get_left() const { return _left; }

DiamondRight::DiamondRight() : _right(0) {}
void DiamondRight::set_right(int val) { _right = val; }
int DiamondRight::get_right() const { return _right; }

DiamondBottom::DiamondBottom() : _bottom(0) {}
void DiamondBottom::set_bottom(int val) { _bottom = val; }
int DiamondBottom::get_bottom() const { return _bottom; }

DiamondTester::DiamondTester() : _last_left(0), _last_right(0) {}
void DiamondTester::test_left(DiamondLeft *obj) { _last_left = obj->get_left(); }
void DiamondTester::test_right(DiamondRight *obj) { _last_right = obj->get_right(); }
int DiamondTester::get_last_left() const { return _last_left; }
int DiamondTester::get_last_right() const { return _last_right; }
