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

// 设置对象的析构函数 / Set object's destructor function
void setDestructor(void* obj, void (*destructor)(void*));

// 分配带 ARC 头的对象 / Allocate object with ARC header
void* arcAlloc(size_t size, void (*destructor)(void*) = nullptr);

// Autorelease pool / 自动释放池
class AutoreleasePool {
public:
    AutoreleasePool();
    ~AutoreleasePool();

    // 添加对象到池 / Add object to pool
    void add(void* obj);

    // 释放池中所有对象 / Release all objects in pool
    void drain();

private:
    struct Impl;
    Impl* impl_;
};

// 全局 autorelease pool / Global autorelease pool
AutoreleasePool* currentAutoreleasePool();

// Initialize the ARC runtime
void arcInit();

// Shutdown the ARC runtime (cleanup weak reference table)
void arcShutdown();

} // namespace suki::runtime
