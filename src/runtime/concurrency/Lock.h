#pragma once
// SukiCode 同步原语运行时实现
// Mutex, RWLock, Semaphore, Condition for thread synchronization.

#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <cstdint>

namespace suki::runtime {

// 互斥锁 / Mutex
class Mutex {
public:
    Mutex() = default;
    ~Mutex() = default;

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    // 加锁 / Lock
    void lock() { mutex_.lock(); }

    // 尝试加锁 / Try lock
    bool tryLock() { return mutex_.try_lock(); }

    // 解锁 / Unlock
    void unlock() { mutex_.unlock(); }

    // 获取底层互斥锁 / Get underlying mutex
    std::mutex& native() { return mutex_; }

private:
    std::mutex mutex_;
};

// 锁守卫 / Lock guard
template<typename Lock>
class LockGuard {
public:
    explicit LockGuard(Lock& lock) : lock_(lock) { lock_.lock(); }
    ~LockGuard() { lock_.unlock(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Lock& lock_;
};

// 读写锁 / Read-Write lock
class RWLock {
public:
    RWLock() = default;
    ~RWLock() = default;

    RWLock(const RWLock&) = delete;
    RWLock& operator=(const RWLock&) = delete;

    // 读锁 / Read lock
    void readLock() { mutex_.lock_shared(); }

    // 尝试读锁 / Try read lock
    bool tryReadLock() { return mutex_.try_lock_shared(); }

    // 解读锁 / Read unlock
    void readUnlock() { mutex_.unlock_shared(); }

    // 写锁 / Write lock
    void writeLock() { mutex_.lock(); }

    // 尝试写锁 / Try write lock
    bool tryWriteLock() { return mutex_.try_lock(); }

    // 解写锁 / Write unlock
    void writeUnlock() { mutex_.unlock(); }

private:
    std::shared_mutex mutex_;
};

// 信号量 / Semaphore
class Semaphore {
public:
    explicit Semaphore(int64_t initialCount = 0) : count_(initialCount) {}
    ~Semaphore() = default;

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    // 等待（减少计数）/ Wait (decrement count)
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return count_ > 0; });
        --count_;
    }

    // 尝试等待 / Try wait
    bool tryWait() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ > 0) {
            --count_;
            return true;
        }
        return false;
    }

    // 信号（增加计数）/ Signal (increment count)
    void signal() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++count_;
        cv_.notify_one();
    }

    // 获取当前计数 / Get current count
    int64_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

private:
    int64_t count_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

// 条件变量 / Condition variable
class Condition {
public:
    Condition() = default;
    ~Condition() = default;

    Condition(const Condition&) = delete;
    Condition& operator=(const Condition&) = delete;

    // 等待 / Wait (需要配合 LockGuard 使用)
    void wait(std::unique_lock<std::mutex>& lock) {
        cv_.wait(lock);
    }

    // 带谓词等待 / Wait with predicate
    template<typename Pred>
    void wait(std::unique_lock<std::mutex>& lock, Pred pred) {
        cv_.wait(lock, pred);
    }

    // 通知一个等待者 / Notify one waiter
    void notifyOne() { cv_.notify_one(); }

    // 通知所有等待者 / Notify all waiters
    void notifyAll() { cv_.notify_all(); }

private:
    std::condition_variable cv_;
};

} // namespace suki::runtime
