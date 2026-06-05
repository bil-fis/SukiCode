#pragma once
// String interning for efficient identifier and string comparison.
// All identifiers and string constants are interned during compilation.

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace suki {

class StringInterner {
public:
    // Intern a string and return its unique ID
    uint32_t intern(std::string_view str) {
        auto it = map_.find(std::string(str));
        if (it != map_.end()) return it->second;

        uint32_t id = static_cast<uint32_t>(strings_.size());
        strings_.emplace_back(str);
        map_[strings_.back()] = id;
        return id;
    }

    // Look up a string by ID
    std::string_view lookup(uint32_t id) const {
        if (id >= strings_.size()) return "";
        return strings_[id];
    }

    // Check if a string is already interned
    bool contains(std::string_view str) const {
        return map_.count(std::string(str)) > 0;
    }

    size_t size() const { return strings_.size(); }

private:
    std::vector<std::string> strings_;
    std::unordered_map<std::string, uint32_t> map_;
};

} // namespace suki
