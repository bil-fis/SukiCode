#pragma once
// ARC (Automatic Reference Counting) runtime for SukiCode.
// Provides retain/release/autorelease for class instances.

#include <cstdint>
#include <atomic>

namespace suki::runtime {

// ARC object header — embedded at the start of every class instance
struct ARCHeader {
    std::atomic<int32_t> refCount;
    // Weak reference count (separate from strong)
    std::atomic<int32_t> weakRefCount;
    // Destructor function pointer (called when strong refcount reaches 0)
    void (*destructor)(void* obj);

    ARCHeader() : refCount(1), weakRefCount(0), destructor(nullptr) {}
};

// ARC operations
void retain(void* obj);
void release(void* obj);
void* weakRetain(void* obj);
void weakRelease(void* obj);

// Initialize the ARC runtime
void arcInit();

// Shutdown the ARC runtime (cleanup weak reference table)
void arcShutdown();

} // namespace suki::runtime
