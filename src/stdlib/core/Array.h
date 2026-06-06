#pragma once
// SukiCode Array<T> 值类型 COW 数组
// Value-type array with Copy-On-Write optimization.

#include <vector>
#include <memory>
#include <cstddef>
#include <functional>
#include <initializer_list>

namespace suki::stdlib {

template<typename T>
class Array {
public:
    // 构造函数 / Constructors
    Array() : data_(std::make_shared<std::vector<T>>()) {}
    Array(std::initializer_list<T> init) : data_(std::make_shared<std::vector<T>>(init)) {}
    Array(size_t count, const T& value) : data_(std::make_shared<std::vector<T>>(count, value)) {}

    // 拷贝构造（COW）/ Copy constructor (COW)
    Array(const Array& other) : data_(other.data_) {}

    // 移动构造 / Move constructor
    Array(Array&& other) noexcept : data_(std::move(other.data_)) {}

    // 赋值 / Assignment
    Array& operator=(const Array& other) { data_ = other.data_; return *this; }
    Array& operator=(Array&& other) noexcept { data_ = std::move(other.data_); return *this; }

    // 大小 / Size
    size_t count() const { return data_->size(); }
    size_t size() const { return data_->size(); }
    bool isEmpty() const { return data_->empty(); }

    // 访问 / Access
    const T& operator[](size_t index) const { return (*data_)[index]; }
    T& operator[](size_t index) { ensureUnique(); return (*data_)[index]; }

    const T& first() const { return data_->front(); }
    const T& last() const { return data_->back(); }

    // 修改 / Modification
    void append(const T& value) { ensureUnique(); data_->push_back(value); }
    void append(T&& value) { ensureUnique(); data_->push_back(std::move(value)); }
    void insert(size_t index, const T& value) { ensureUnique(); data_->insert(data_->begin() + index, value); }
    void remove(size_t index) { ensureUnique(); data_->erase(data_->begin() + index); }
    void removeAll() { ensureUnique(); data_->clear(); }

    // 查找 / Search
    bool contains(const T& value) const {
        for (const auto& item : *data_) {
            if (item == value) return true;
        }
        return false;
    }

    size_t firstIndex(const T& value) const {
        for (size_t i = 0; i < data_->size(); i++) {
            if ((*data_)[i] == value) return i;
        }
        return npos;
    }

    // 映射 / Map
    template<typename U>
    Array<U> map(std::function<U(const T&)> transform) const {
        Array<U> result;
        for (const auto& item : *data_) {
            result.append(transform(item));
        }
        return result;
    }

    // 过滤 / Filter
    Array filter(std::function<bool(const T&)> predicate) const {
        Array result;
        for (const auto& item : *data_) {
            if (predicate(item)) result.append(item);
        }
        return result;
    }

    // 归约 / Reduce
    T reduce(const T& initial, std::function<T(const T&, const T&)> combiner) const {
        T result = initial;
        for (const auto& item : *data_) {
            result = combiner(result, item);
        }
        return result;
    }

    // 排序 / Sort
    void sort() { ensureUnique(); std::sort(data_->begin(), data_->end()); }

    // 反转 / Reverse
    void reverse() { ensureUnique(); std::reverse(data_->begin(), data_->end()); }

    // 迭代 / Iteration
    auto begin() const { return data_->begin(); }
    auto end() const { return data_->end(); }
    auto begin() { ensureUnique(); return data_->begin(); }
    auto end() { ensureUnique(); return data_->end(); }

    // 比较 / Comparison
    bool operator==(const Array& other) const { return *data_ == *other.data_; }
    bool operator!=(const Array& other) const { return *data_ != *other.data_; }

    static constexpr size_t npos = static_cast<size_t>(-1);

private:
    void ensureUnique() {
        if (!data_ || data_.use_count() > 1) {
            data_ = std::make_shared<std::vector<T>>(*data_);
        }
    }

    std::shared_ptr<std::vector<T>> data_;
};

} // namespace suki::stdlib
