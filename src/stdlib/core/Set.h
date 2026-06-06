#pragma once
// SukiCode Set<T> - 无序唯一集合
// Unordered unique collection.

#include <unordered_set>
#include <vector>
#include <functional>
#include <initializer_list>

namespace suki::stdlib {

template<typename T>
class Set {
public:
    Set() = default;
    Set(std::initializer_list<T> init) : data_(init) {}

    // 拷贝/移动 / Copy/Move
    Set(const Set&) = default;
    Set(Set&&) = default;
    Set& operator=(const Set&) = default;
    Set& operator=(Set&&) = default;

    // 插入 / Insert
    bool insert(const T& value) {
        return data_.insert(value).second;
    }

    // 移除 / Remove
    bool remove(const T& value) {
        return data_.erase(value) > 0;
    }

    // 包含 / Contains
    bool contains(const T& value) const {
        return data_.count(value) > 0;
    }

    // 大小 / Size
    size_t count() const { return data_.size(); }
    bool isEmpty() const { return data_.empty(); }

    // 清空 / Clear
    void clear() { data_.clear(); }

    // 集合操作 / Set operations
    Set unionWith(const Set& other) const {
        Set result = *this;
        for (const auto& item : other.data_) {
            result.data_.insert(item);
        }
        return result;
    }

    Set intersect(const Set& other) const {
        Set result;
        for (const auto& item : data_) {
            if (other.contains(item)) {
                result.data_.insert(item);
            }
        }
        return result;
    }

    Set subtract(const Set& other) const {
        Set result;
        for (const auto& item : data_) {
            if (!other.contains(item)) {
                result.data_.insert(item);
            }
        }
        return result;
    }

    // 转换为数组 / Convert to array
    std::vector<T> toArray() const {
        return std::vector<T>(data_.begin(), data_.end());
    }

    // 迭代 / Iteration
    auto begin() const { return data_.begin(); }
    auto end() const { return data_.end(); }

    // 比较 / Comparison
    bool operator==(const Set& other) const { return data_ == other.data_; }
    bool operator!=(const Set& other) const { return data_ != other.data_; }

private:
    std::unordered_set<T> data_;
};

} // namespace suki::stdlib
