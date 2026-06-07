#pragma once
// SukiCode TaskGroup - 结构化并发
// Structured concurrency with TaskGroup and Task.

#include <functional>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <memory>
#include <stdexcept>

namespace suki::stdlib {

// 取消错误 / Cancellation error
class CancellationError : public std::runtime_error {
public:
    CancellationError() : std::runtime_error("task was cancelled") {}
};

// 任务句柄 / Task handle
template<typename T>
class Task {
public:
    Task() : cancelled_(std::make_shared<std::atomic<bool>>(false)) {}

    // 取消任务 / Cancel task
    void cancel() { *cancelled_ = true; }

    // 是否已取消 / Is cancelled
    bool isCancelled() const { return *cancelled_; }

    // 获取取消令牌 / Get cancellation token
    std::shared_ptr<std::atomic<bool>> cancellationToken() const { return cancelled_; }

private:
    std::shared_ptr<std::atomic<bool>> cancelled_;
};

// 任务组 / Task group
template<typename T>
class TaskGroup {
public:
    using TaskFunc = std::function<T()>;

    TaskGroup() = default;
    ~TaskGroup() { wait(); }

    // 禁止拷贝/移动 / No copy/move
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;

    // 添加任务 / Add task
    void addTask(TaskFunc task) {
        auto token = cancelToken_;
        auto future = std::async(std::launch::async, [task, token]() -> T {
            if (*token) throw CancellationError();
            return task();
        });
        std::lock_guard<std::mutex> lock(mutex_);
        futures_.push_back(std::move(future));
    }

    // 等待所有任务完成 / Wait for all tasks to complete
    void wait() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& f : futures_) {
            try {
                f.get();
            } catch (const CancellationError&) {
                // 忽略取消错误
            }
        }
        futures_.clear();
    }

    // 取消所有任务 / Cancel all tasks
    void cancel() {
        *cancelToken_ = true;
    }

    // 获取结果 / Get results
    std::vector<T> results() {
        std::vector<T> res;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& f : futures_) {
            try {
                res.push_back(f.get());
            } catch (...) {
                // 跳过失败的任务
            }
        }
        futures_.clear();
        return res;
    }

    // 是否有任务失败 / Has any task failed
    bool hasErrors() const {
        for (auto& f : futures_) {
            if (f.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                    f.get();
                } catch (...) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    std::mutex mutex_;
    std::vector<std::future<T>> futures_;
    std::shared_ptr<std::atomic<bool>> cancelToken_ = std::make_shared<std::atomic<bool>>(false);
};

// withTaskGroup 辅助函数 / withTaskGroup helper
template<typename T>
void withTaskGroup(std::function<void(TaskGroup<T>&)> body) {
    TaskGroup<T> group;
    body(group);
    group.wait();
}

} // namespace suki::stdlib
