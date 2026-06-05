#pragma once
// Scope management for name resolution.

#include <string>
#include <unordered_map>
#include <memory>

namespace suki {

class Scope {
public:
    Scope(std::shared_ptr<Scope> parent = nullptr);
    ~Scope();

    // Define a name in this scope
    void define(const std::string& name, void* decl);

    // Look up a name, searching parent scopes
    void* lookup(const std::string& name) const;

    // Check if name exists in this scope only (not parents)
    bool contains(const std::string& name) const;

    std::shared_ptr<Scope> parent() const { return parent_; }

private:
    std::unordered_map<std::string, void*> bindings_;
    std::shared_ptr<Scope> parent_;
};

} // namespace suki
