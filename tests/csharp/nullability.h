//FLAGS: -promiscuous -string -refcount
#include <string>

class Resource {
public:
  Resource(const std::string &name);
  std::string get_name() const;
};

class ResourceManager {
public:
  ResourceManager();

  [[in::nullable]] Resource *find_resource(const std::string &name) const;
  Resource *get_default_resource() const;
  void set_active([[in::nullable]] Resource *resource);
  void set_alias([[in::nullable]] const char *alias);
  [[in::nullable]] const char *find_label(const std::string &name) const;
  const char *default_label() const;
};
