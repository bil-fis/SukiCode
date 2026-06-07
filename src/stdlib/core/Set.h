#pragma once
// SukiCode Set<T> - 无序唯一集合 (COW)
// Unordered unique collection with Copy-On-Write.

#include <unordered_set>
#include <vector>
#include <functional>
#include <initializer_list>
#include <memory>

namespace suki::stdlib {

template<typename T>
class Set {
public:
    Set() : data_(std::make_shared<std::unordered_set<T>>()) {}
    Set(std::initializer_list<T> init) : data_(std::make_shared<std::unordered_set<T>>(init)) {}

    // 拷贝构造（COW）/ Copy constructor (COW)
    Set(const Set& other) : data_(other.data_) {}

    // 移动构造 / Move constructor
    Set(Set&& other) noexcept : data_(std::move(other.data_)) {}

    // 赋值 / Assignment
    Set& operator=(const Set& other) { data_ = other.data_; return *this; }
    Set& operator=(Set&& other) noexcept { data_ = std::move(other.data_); return *this; }

    // 插入 / Insert
    bool insert(const T& value) {
        ensureUnique();
        return data_->insert(value).second;
    }

    // 移除 / Remove
    bool remove(const T& value) {
        ensureUnique();
        return data_->erase(value) > 0;
    }

    // 包含 / Contains
    bool contains(const T& value) const {
        return data_->count(value) > 0;
    }

    // 大小 / Size
    size_t count() const { return data_->size(); }
    bool isEmpty() const { return data_->empty(); }

    // 清空 / Clear
    void clear() { ensureUnique(); data_->clear(); }

    // 集合操作 / Set operations
    Set unionWith(const Set& other) const {
        Set result = *this;
        for (const auto& item : *other.data_) {
            result.data_->insert(item);
        }
        return result;
    }

    Set intersect(const Set& other) const {
        Set result;
        for (const auto& item : *data_) {
            if (other.contains(item)) {
                result.data_->insert(item);
            }
        }
        return result;
    }

    Set subtract(const Set& other) const {
        Set result;
        for (const auto& item : *data_) {
            if (!other.contains(item)) {
                result.data_->insert(item);
            }
        }
        return result;
    }

    // 转换为数组 / Convert to array
    std::vector<T> toArray() const {
        return std::vector<T>(data_->begin(), data_->end());
    }

    // 是否是子集 / Is subset
    bool isSubsetOf(const Set& other) const {
        for (const auto& item : *data_) {
            if (!other.contains(item)) return false;
        }
        return true;
    }

    // 是否是超集 / Is superset
    bool isSupersetOf(const Set& other) const {
        return other.isSubsetOf(*this);
    }

    // 是否不相交 / Is disjoint
    bool isDisjointWith(const Set& other) const {
        for (const auto& item : *data_) {
            if (other.contains(item)) return false;
        }
        return true;
    }

    // 对称差集 / Symmetric difference
    Set symmetricDifference(const Set& other) const {
        Set result;
        for (const auto& item : *data_) {
            if (!other.contains(item)) result.insert(item);
        }
        for (const auto& item : *other.data_) {
            if (!contains(item)) result.insert(item);
        }
        return result;
    }

    // 迭代 / Iteration
    auto begin() const { return data_->begin(); }
    auto end() const { return data_->end(); }

    // 比较 / Comparison
    bool operator==(const Set& other) const { return *data_ == *other.data_; }
    bool operator!=(const Set& other) const { return *data_ != *other.data_; }

private:
    void ensureUnique() {
        if (!data_ || data_.use_count() > 1) {
            data_ = std::make_shared<std::unordered_set<T>>(*data_);
        }
    }

    std::shared_ptr<std::unordered_set<T>> data_;
};

} // namespace suki::stdlib
