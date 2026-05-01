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
