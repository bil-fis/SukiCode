// ThreadPool implementation.
#include "ThreadPool.h"
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace suki::runtime {

struct ThreadPool::Impl {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    std::atomic<int> activeTasks{0};

    Impl(size_t numWorkers) {
        if (numWorkers == 0) {
            numWorkers = std::thread::hardware_concurrency();
            if (numWorkers == 0) numWorkers = 4;
        }

        for (size_t i = 0; i < numWorkers; i++) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [this] {
                            return stop.load() || !tasks.empty();
                        });
                        if (stop.load() && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                    activeTasks.fetch_sub(1);
                }
            });
        }
    }

    ~Impl() {
        stop.store(true);
        cv.notify_all();
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }
};

ThreadPool::ThreadPool(size_t numWorkers)
    : impl_(std::make_unique<Impl>(numWorkers)) {}

ThreadPool::~ThreadPool() = default;

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->tasks.push(std::move(task));
        impl_->activeTasks.fetch_add(1);
    }
    impl_->cv.notify_one();
}

template<typename F>
auto ThreadPool::submitWithResult(F&& func) -> std::future<decltype(func())> {
    using ReturnType = decltype(func());
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(std::forward<F>(func));
    std::future<ReturnType> result = task->get_future();
    submit([task]() { (*task)(); });
    return result;
}

void ThreadPool::waitForAll() {
    while (impl_->activeTasks.load() > 0) {
        std::this_thread::yield();
    }
}

size_t ThreadPool::workerCount() const {
    return impl_->workers.size();
}

} // namespace suki::runtime
