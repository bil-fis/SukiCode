#pragma once
// SukiCode COW (Copy-on-Write) 内部存储
// COW internal storage for value types.

#include <memory>
#include <atomic>
#include <functional>

namespace suki::stdlib {

// COW 存储包装器 / COW storage wrapper
template<typename T>
class COWStorage {
public:
    // 创建新的存储 / Create new storage
    explicit COWStorage(T value)
        : data_(std::make_shared<T>(std::move(value))) {}

    // 拷贝构造（共享引用）/ Copy constructor (shared reference)
    COWStorage(const COWStorage& other) : data_(other.data_) {}

    // 移动构造 / Move constructor
    COWStorage(COWStorage&& other) noexcept : data_(std::move(other.data_)) {}

    // 拷贝赋值（共享引用）/ Copy assignment (shared reference)
    COWStorage& operator=(const COWStorage& other) {
        data_ = other.data_;
        return *this;
    }

    // 移动赋值 / Move assignment
    COWStorage& operator=(COWStorage&& other) noexcept {
        data_ = std::move(other.data_);
        return *this;
    }

    // 获取只读引用 / Get read-only reference
    const T& read() const { return *data_; }

    // 获取可写引用（触发 COW）/ Get mutable reference (triggers COW)
    T& write() {
        ensureUnique();
        return *data_;
    }

    // 检查是否共享 / Check if shared
    bool isShared() const { return data_.use_count() > 1; }

    // 获取引用计数 / Get reference count
    long useCount() const { return data_.use_count(); }

private:
    // 确保唯一所有权 / Ensure unique ownership
    void ensureUnique() {
        if (data_.use_count() > 1) {
            data_ = std::make_shared<T>(*data_);
        }
    }

    std::shared_ptr<T> data_;
};

} // namespace suki::stdlib
