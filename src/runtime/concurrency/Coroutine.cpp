// SukiCode 协程运行时实现
// Coroutine runtime implementation.

#include "Coroutine.h"
#include <mutex>

namespace suki::runtime {

void CoroutineHandle::resume() {
    if (state != CoroutineState::Suspended && state != CoroutineState::Initial) return;
    state = CoroutineState::Running;
    // TODO: 实际恢复协程执行 / Actually resume coroutine execution
}

void CoroutineHandle::destroy() {
    state = CoroutineState::Completed;
    address = nullptr;
}

// ─── CoroutineScheduler ─────────────────────────────────────────────────

CoroutineScheduler& CoroutineScheduler::instance() {
    static CoroutineScheduler scheduler;
    return scheduler;
}

void CoroutineScheduler::schedule(CoroutineHandle handle) {
    pending_.push_back(handle);
}

void CoroutineScheduler::runAll() {
    // 简单实现：依次执行所有待处理的协程
    // Simple implementation: execute all pending coroutines
    while (!pending_.empty()) {
        auto handle = pending_.back();
        pending_.pop_back();
        if (!handle.done()) {
            handle.resume();
        }
    }
}

size_t CoroutineScheduler::pendingCount() const {
    return pending_.size();
}

// ─── createCoroutine ────────────────────────────────────────────────────

CoroutineHandle createCoroutine(void* frame) {
    CoroutineHandle handle;
    handle.address = frame;
    handle.state = CoroutineState::Initial;
    return handle;
}

} // namespace suki::runtime
