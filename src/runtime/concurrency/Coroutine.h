#pragma once
// Coroutine runtime for SukiCode async/await.
// Implements stackless coroutines (similar to C++20 coroutines).

#include <cstdint>
#include <functional>

namespace suki::runtime {

// Coroutine state machine states
enum class CoroutineState : uint8_t {
    Initial,
    Suspended,
    Running,
    Completed,
    Error,
};

// Coroutine handle — represents a suspended coroutine
struct CoroutineHandle {
    void* address;
    bool done() const;

    // Resume the coroutine
    void resume();

    // Destroy the coroutine
    void destroy();
};

// Awaiter interface
struct Awaiter {
    virtual ~Awaiter() = default;
    virtual bool await_ready() const { return false; }
    virtual void await_suspend(CoroutineHandle handle) {}
    virtual void await_resume() {}
};

} // namespace suki::runtime
