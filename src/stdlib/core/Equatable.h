#pragma once
// SukiCode Equatable 和 Hashable 协议
// Equatable and Hashable protocols for type comparison.

#include <cstdint>
#include <functional>

namespace suki::stdlib {

// Equatable 协议 / Equatable protocol
// 支持相等比较的类型必须实现此接口
// Types that support equality comparison must implement this interface
class Equatable {
public:
    virtual ~Equatable() = default;

    // 相等比较 / Equality comparison
    virtual bool equals(const Equatable& other) const = 0;

    // 不等比较 / Inequality comparison
    bool notEquals(const Equatable& other) const {
        return !equals(other);
    }

    // 操作符重载 / Operator overloads
    bool operator==(const Equatable& other) const { return equals(other); }
    bool operator!=(const Equatable& other) const { return notEquals(other); }
};

// Comparable 协议 / Comparable protocol
// 支持排序比较的类型必须实现此接口
// Types that support ordering comparison must implement this interface
class Comparable : public Equatable {
public:
    // 比较 / Compare
    // 返回: -1 (a < b), 0 (a == b), 1 (a > b)
    virtual int compare(const Comparable& other) const = 0;

    // 操作符重载 / Operator overloads
    bool operator<(const Comparable& other) const { return compare(other) < 0; }
    bool operator>(const Comparable& other) const { return compare(other) > 0; }
    bool operator<=(const Comparable& other) const { return compare(other) <= 0; }
    bool operator>=(const Comparable& other) const { return compare(other) >= 0; }
};

// Hashable 协议 / Hashable protocol
// 支持哈希的类型必须实现此接口
// Types that support hashing must implement this interface
class Hashable : public Equatable {
public:
    // 获取哈希值 / Get hash value
    virtual size_t hash() const = 0;
};

// 哈希组合工具 / Hash combination utility
inline size_t hashCombine(size_t seed, size_t value) {
    seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    return seed;
}

// 基本类型的哈希特化 / Hash specializations for primitive types
template<typename T>
struct Hash {
    size_t operator()(const T& value) const {
        return std::hash<T>{}(value);
    }
};

} // namespace suki::stdlib
