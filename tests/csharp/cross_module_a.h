//FLAGS: -promiscuous -string -refcount
// Module A - defines base classes
#include <string>

class Serializable {
__published:
  Serializable();
  virtual ~Serializable();
  virtual std::string serialize() const;
};

class Printable {
__published:
  Printable();
  virtual ~Printable();
  virtual std::string to_string() const;
};
