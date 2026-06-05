#pragma once
// SukiCode 协程运行时
// Coroutine runtime for async/await support.
// Implements stackless coroutines using function pointers and continuations.

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace suki::runtime {

// 协程状态 / Coroutine state
enum class CoroutineState : uint8_t {
    Initial,    // 初始状态
    Suspended,  // 已挂起
    Running,    // 运行中
    Completed,  // 已完成
    Error,      // 出错
};

// 协程句柄 / Coroutine handle
struct CoroutineHandle {
    void* address;       // 协程帧地址
    CoroutineState state;

    bool done() const { return state == CoroutineState::Completed || state == CoroutineState::Error; }
    void resume();       // 恢复执行
    void destroy();      // 销毁协程
};

// Awaiter 接口 / Awaiter interface
struct Awaiter {
    virtual ~Awaiter() = default;
    virtual bool await_ready() const { return false; }
    virtual void await_suspend(CoroutineHandle handle) {}
    virtual void await_resume() {}
};

// Promise 基类 / Promise base class
template<typename T>
struct Promise {
    virtual ~Promise() = default;
    virtual T get_result() = 0;
    virtual bool is_ready() const = 0;
};

// Future 类型 / Future type
template<typename T>
class Future {
public:
    Future() : ready_(false) {}
    Future(T value) : ready_(true), value_(std::move(value)) {}

    bool isReady() const { return ready_; }
    T get() const { return value_; }

    void set(T value) {
        value_ = std::move(value);
        ready_ = true;
    }

private:
    bool ready_;
    T value_;
};

// 协程调度器 / Coroutine scheduler
class CoroutineScheduler {
public:
    static CoroutineScheduler& instance();

    // 调度协程执行 / Schedule coroutine for execution
    void schedule(CoroutineHandle handle);

    // 运行所有待处理的协程 / Run all pending coroutines
    void runAll();

    // 待处理协程数量 / Number of pending coroutines
    size_t pendingCount() const;

private:
    std::vector<CoroutineHandle> pending_;
};

// 创建协程句柄 / Create coroutine handle
CoroutineHandle createCoroutine(void* frame);

} // namespace suki::runtime
