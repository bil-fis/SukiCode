#pragma once
// MemoryPool<T> — batch allocation of fixed-size objects.
// Bypasses per-object ARC for high-performance scenarios.

#include <cstddef>
#include <vector>
#include <mutex>

namespace suki::runtime {

template<typename T>
class MemoryPool {
public:
    explicit MemoryPool(size_t capacity)
        : capacity_(capacity), allocated_(0) {
        storage_.resize(capacity);
    }

    ~MemoryPool() {
        releaseAll();
    }

    // Allocate an object from the pool (thread-safe)
    T* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (allocated_ >= capacity_) return nullptr;
        return &storage_[allocated_++];
    }

    // Release all allocated objects
    void releaseAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        allocated_ = 0;
    }

    size_t capacity() const { return capacity_; }
    size_t allocatedCount() const { return allocated_; }

private:
    size_t capacity_;
    size_t allocated_;
    std::vector<T> storage_;
    std::mutex mutex_;
};

} // namespace suki::runtime
