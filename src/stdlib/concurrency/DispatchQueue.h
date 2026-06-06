#pragma once
// SukiCode DispatchQueue - 串行/并行调度队列
// Serial and concurrent dispatch queues.

#include <functional>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>

namespace suki::stdlib {

// 调度队列类型 / Dispatch queue type
enum class DispatchQueueType {
    Serial,     // 串行：一次只执行一个任务
    Concurrent, // 并行：同时执行多个任务
};

// 调度队列 / Dispatch queue
class DispatchQueue {
public:
    using Task = std::function<void()>;

    // 创建队列 / Create queue
    DispatchQueue(const std::string& label, DispatchQueueType type = DispatchQueueType::Serial,
                  size_t workerCount = 1)
        : label_(label), type_(type), running_(true) {
        size_t threads = (type == DispatchQueueType::Concurrent) ? workerCount : 1;
        for (size_t i = 0; i < threads; i++) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    // 析构时等待所有任务完成 / Wait for all tasks on destruction
    ~DispatchQueue() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            running_ = false;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    // 禁止拷贝/移动 / No copy/move
    DispatchQueue(const DispatchQueue&) = delete;
    DispatchQueue& operator=(const DispatchQueue&) = delete;

    // 异步调度 / Async dispatch
    void async(Task task) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    // 同步调度（等待完成）/ Sync dispatch (wait for completion)
    void sync(Task task) {
        std::atomic<bool> done{false};
        std::exception_ptr ex;
        async([&]() {
            try {
                task();
            } catch (...) {
                ex = std::current_exception();
            }
            done = true;
        });
        // 等待完成
        while (!done) {
            std::this_thread::yield();
        }
        if (ex) std::rethrow_exception(ex);
    }

    // 延迟调度 / Dispatch after delay
    void dispatchAfter(double delaySeconds, Task task) {
        std::thread([this, delaySeconds, task = std::move(task)]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<int>(delaySeconds * 1000)));
            async(std::move(task));
        }).detach();
    }

    // 获取队列标签 / Get queue label
    const std::string& label() const { return label_; }

    // 获取待处理任务数 / Get pending task count
    size_t pendingCount() {
        std::unique_lock<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    // 主队列（串行）/ Main queue (serial)
    static DispatchQueue& main() {
        static DispatchQueue queue("main", DispatchQueueType::Serial);
        return queue;
    }

    // 全局队列（并行）/ Global queue (concurrent)
    static DispatchQueue& global() {
        static DispatchQueue queue("global", DispatchQueueType::Concurrent,
                                  std::thread::hardware_concurrency());
        return queue;
    }

private:
    void workerLoop() {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return !running_ || !tasks_.empty(); });
                if (!running_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            // 串行队列需要在锁外执行，但保证顺序
            if (type_ == DispatchQueueType::Serial) {
                task();
            } else {
                task();
            }
        }
    }

    std::string label_;
    DispatchQueueType type_;
    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_;
};

} // namespace suki::stdlib
