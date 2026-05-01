#include "nullability.h"

Resource::Resource(const std::string &name) : _name(name) {}
std::string Resource::get_name() const { return _name; }

ResourceManager::ResourceManager()
  : _default(new Resource("default")), _active(nullptr) {}

ResourceManager::~ResourceManager() {
  delete _default;
}

Resource *ResourceManager::find_resource(const std::string &name) const {
  // Always returns null for this test — the driver just needs a nullable
  // return that actually produces null.
  (void)name;
  return nullptr;
}

Resource *ResourceManager::get_default_resource() const {
  return _default;
}

void ResourceManager::set_active(Resource *resource) {
  _active = resource;
}

std::string ResourceManager::active_name() const {
  return _active != nullptr ? _active->get_name() : std::string();
}
