#pragma once

// Per-scope hash table used by semantic analysis for name resolution.
//
// Names are declared into the innermost scope and looked up from the
// innermost outward (so inner declarations shadow outer ones). A single
// lookup is O(number of enclosing scopes); scoping keeps the common case
// (a local name) shallow and avoids the O(N) repeated scans a single flat
// map would require when walking large bodies.

#include <string>
#include <unordered_map>
#include <vector>

namespace suki {

template <typename V>
class ScopedTable {
public:
    void pushScope() { scopes_.emplace_back(); }
    void popScope() { if (!scopes_.empty()) scopes_.pop_back(); }
    size_t depth() const { return scopes_.size(); }

    // Declare a name in the innermost scope, shadowing any outer binding.
    void declare(const std::string& name, V value) {
        if (scopes_.empty()) scopes_.emplace_back();
        scopes_.back()[name] = std::move(value);
    }

    // Find the innermost binding for `name`, or nullptr.
    const V* lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    // True if `name` is declared in the innermost scope (redeclaration check).
    bool declaredHere(const std::string& name) const {
        if (scopes_.empty()) return false;
        auto f = scopes_.back().find(name);
        return f != scopes_.back().end();
    }

    void clear() { scopes_.clear(); }

private:
    std::vector<std::unordered_map<std::string, V>> scopes_;
};

} // namespace suki
