#pragma once
// Weak reference implementation for SukiCode ARC.
#include "ARC.h"
#include <atomic>

namespace suki::runtime {

// WeakRef<T> — a weak reference that zeroes when the object is deallocated
template<typename T>
class WeakRef {
public:
    WeakRef() : ptr_(nullptr) {}
    explicit WeakRef(T* p) : ptr_(p) {
        if (ptr_) weakRetain(ptr_);
    }
    ~WeakRef() {
        if (ptr_) weakRelease(ptr_);
    }

    WeakRef(const WeakRef& other) : ptr_(other.ptr_) {
        if (ptr_) weakRetain(ptr_);
    }
    WeakRef& operator=(const WeakRef& other) {
        if (this != &other) {
            if (ptr_) weakRelease(ptr_);
            ptr_ = other.ptr_;
            if (ptr_) weakRetain(ptr_);
        }
        return *this;
    }

    WeakRef(WeakRef&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    WeakRef& operator=(WeakRef&& other) noexcept {
        if (this != &other) {
            if (ptr_) weakRelease(ptr_);
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* get() const {
        // Attempt to lock — returns nullptr if object is dead
        if (!ptr_) return nullptr;
        auto* header = reinterpret_cast<ARCHeader*>(ptr_);
        if (header->refCount.load(std::memory_order_acquire) == 0) {
            return nullptr;
        }
        return ptr_;
    }

    T* operator->() const { return get(); }
    explicit operator bool() const { return get() != nullptr; }

private:
    T* ptr_;
};

} // namespace suki::runtime
