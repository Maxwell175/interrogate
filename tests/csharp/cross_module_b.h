//FLAGS: -promiscuous -string -refcount
// Module B - uses base classes from Module A
#include <string>
#include "cross_module_a.h"

class Document : public Serializable, public Printable {
__published:
  Document(const std::string &title);
  virtual ~Document();
  std::string get_title() const;
  __make_property(title, get_title);
  virtual std::string serialize() const;
  virtual std::string to_string() const;
};
