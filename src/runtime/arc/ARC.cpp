// ARC runtime implementation.
#include "ARC.h"
#include <unordered_set>
#include <vector>
#include <mutex>

namespace suki::runtime {

// Global set of objects with weak references (for zeroing weak pointers)
static std::unordered_set<void*> g_weakObjects;
static std::mutex g_weakMutex;

void retain(void* obj) {
    if (!obj) return;
    auto* header = reinterpret_cast<ARCHeader*>(obj);
    header->refCount.fetch_add(1, std::memory_order_relaxed);
}

void release(void* obj) {
    if (!obj) return;
    auto* header = reinterpret_cast<ARCHeader*>(obj);
    int32_t newCount = header->refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (newCount == 0) {
        // Call destructor if registered
        if (header->destructor) {
            header->destructor(obj);
        }
        // Zero weak references
        {
            std::lock_guard<std::mutex> lock(g_weakMutex);
            g_weakObjects.erase(obj);
        }
        // Free memory
        ::operator delete(obj);
    }
}

void* weakRetain(void* obj) {
    if (!obj) return nullptr;
    auto* header = reinterpret_cast<ARCHeader*>(obj);
    // Check if object is still alive
    if (header->refCount.load(std::memory_order_acquire) == 0) {
        return nullptr;
    }
    header->weakRefCount.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_weakMutex);
        g_weakObjects.insert(obj);
    }
    return obj;
}

void weakRelease(void* obj) {
    if (!obj) return;
    auto* header = reinterpret_cast<ARCHeader*>(obj);
    header->weakRefCount.fetch_sub(1, std::memory_order_relaxed);
}

void setDestructor(void* obj, void (*destructor)(void*)) {
    if (!obj) return;
    auto* header = reinterpret_cast<ARCHeader*>(obj);
    header->destructor = destructor;
}

void* arcAlloc(size_t size, void (*destructor)(void*)) {
    // 分配内存：ARCHeader + 用户数据
    void* mem = ::operator new(sizeof(ARCHeader) + size);
    auto* header = new (mem) ARCHeader();
    header->destructor = destructor;
    // 返回 ARCHeader 之后的地址
    return reinterpret_cast<char*>(mem) + sizeof(ARCHeader);
}

// ─── AutoreleasePool ────────────────────────────────────────────────────

struct AutoreleasePool::Impl {
    std::vector<void*> objects;
    std::mutex mutex;
};

AutoreleasePool::AutoreleasePool() : impl_(new Impl()) {}
AutoreleasePool::~AutoreleasePool() { drain(); delete impl_; }

void AutoreleasePool::add(void* obj) {
    if (!obj) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->objects.push_back(obj);
}

void AutoreleasePool::drain() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (void* obj : impl_->objects) {
        release(obj);
    }
    impl_->objects.clear();
}

static AutoreleasePool* g_currentPool = nullptr;

AutoreleasePool* currentAutoreleasePool() {
    return g_currentPool;
}

void arcInit() {
    // Initialize ARC runtime state
}

void arcShutdown() {
    // Cleanup
    std::lock_guard<std::mutex> lock(g_weakMutex);
    g_weakObjects.clear();
}

} // namespace suki::runtime
