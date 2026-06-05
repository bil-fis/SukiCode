#pragma once
// ThreadPool — work-stealing thread pool for SukiCode concurrency.

#include <functional>
#include <future>
#include <memory>

namespace suki::runtime {

class ThreadPool {
public:
    explicit ThreadPool(size_t numWorkers = 0); // 0 = auto-detect
    ~ThreadPool();

    // Submit a task (no return value)
    void submit(std::function<void()> task);

    // Submit a task with a return value
    template<typename F>
    auto submitWithResult(F&& func) -> std::future<decltype(func())>;

    // Wait for all tasks to complete
    void waitForAll();

    size_t workerCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace suki::runtime
