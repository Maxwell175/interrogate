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
