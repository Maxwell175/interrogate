//FLAGS: -promiscuous -string -refcount
#include <string>

// Exercises [[in::nullable]] attributes on parameters and returns.
// interrogate emits ? / null-checking on the managed side.

class Resource {
public:
  Resource(const std::string &name);
  std::string get_name() const;

private:
  std::string _name;
};

class ResourceManager {
public:
  ResourceManager();
  ~ResourceManager();

  // Nullable return — absent resources legitimately come back as null.
  [[in::nullable]] Resource *find_resource(const std::string &name) const;

  // Non-nullable return — always produces a result.
  Resource *get_default_resource() const;

  // Nullable parameter — callers may pass null to clear.
  void set_active([[in::nullable]] Resource *resource);

  // Returns the name of whatever is currently active, or an empty string.
  std::string active_name() const;

private:
  Resource *_default;
  Resource *_active;
};
