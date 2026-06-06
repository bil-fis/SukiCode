// SukiCode 协程运行时实现
// Coroutine runtime implementation.

#include "Coroutine.h"
#include <mutex>
#include <csetjmp>

namespace suki::runtime {

// 协程上下文 / Coroutine context
struct CoroutineContext {
    std::jmp_buf jumpBuf;
    bool hasContext = false;
};

void CoroutineHandle::resume() {
    if (state != CoroutineState::Suspended && state != CoroutineState::Initial) return;
    state = CoroutineState::Running;

    // 恢复协程执行 / Resume coroutine execution
    // 如果有保存的上下文，使用 longjmp 恢复
    // If there is a saved context, use longjmp to resume
    if (address) {
        auto* ctx = static_cast<CoroutineContext*>(address);
        if (ctx->hasContext) {
            // 使用 longjmp 恢复到协程暂停点
            // Use longjmp to resume at coroutine suspension point
            std::longjmp(ctx->jumpBuf, 1);
        }
    }

    // 如果没有保存的上下文，协程已完成
    // If no saved context, coroutine is complete
    state = CoroutineState::Completed;
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
