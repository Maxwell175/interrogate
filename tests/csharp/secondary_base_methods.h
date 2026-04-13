//FLAGS: -promiscuous -string -refcount
#include <string>

class PrimaryBase {
public:
  PrimaryBase();
  virtual ~PrimaryBase();
  int get_id() const;
};

class TextLike {
public:
  TextLike();
  virtual ~TextLike();
  void set_text(const std::string &text);
  std::string get_text() const;
};

class DerivedTextNode : public PrimaryBase, public TextLike {
public:
  DerivedTextNode();
};
