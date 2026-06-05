// Scope implementation.
#include "Scope.h"

namespace suki {

Scope::Scope(std::shared_ptr<Scope> parent) : parent_(std::move(parent)) {}
Scope::~Scope() = default;

void Scope::define(const std::string& name, void* decl) {
    bindings_[name] = decl;
}

void* Scope::lookup(const std::string& name) const {
    auto it = bindings_.find(name);
    if (it != bindings_.end()) return it->second;
    if (parent_) return parent_->lookup(name);
    return nullptr;
}

bool Scope::contains(const std::string& name) const {
    return bindings_.count(name) > 0;
}

} // namespace suki
