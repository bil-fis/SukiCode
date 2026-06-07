#pragma once
// SukiCode Channel<T> 运行时实现
// Thread-safe channel for inter-task communication.
// Supports bounded and unbounded channels with backpressure policies.

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <memory>
#include <functional>

namespace suki::runtime {

// 背压策略 / Backpressure policies
enum class Backpressure {
    Block,       // 满时阻塞发送
    DropNewest,  // 丢弃新消息
    DropOldest,  // 丢弃旧消息
    Throw,       // 满时抛出异常
};

// Channel 满错误 / Channel full error
struct ChannelFullError {};

template<typename T>
class Channel {
public:
    // 创建有界通道 / Create bounded channel
    explicit Channel(size_t capacity, Backpressure bp = Backpressure::Block)
        : capacity_(capacity), backpressure_(bp), closed_(false) {}

    // 创建无界通道 / Create unbounded channel
    Channel() : capacity_(0), backpressure_(Backpressure::Block), closed_(false) {}

    ~Channel() { close(); }

    // 发送值到通道 / Send value to channel
    // 返回 false 如果通道已关闭
    bool send(T value) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (closed_) return false;

        // 有界通道检查 / Bounded channel check
        if (capacity_ > 0 && queue_.size() >= capacity_) {
            switch (backpressure_) {
                case Backpressure::Block:
                    // 等待直到有空间或关闭
                    notFull_.wait(lock, [this] {
                        return closed_ || queue_.size() < capacity_;
                    });
                    if (closed_) return false;
                    break;
                case Backpressure::DropNewest:
                    return true; // 丢弃新消息
                case Backpressure::DropOldest:
                    if (!queue_.empty()) queue_.pop(); // 丢弃旧消息
                    break;
                case Backpressure::Throw:
                    return false; // 发送失败
            }
        }

        queue_.push(std::move(value));
        notEmpty_.notify_one();
        return true;
    }

    // 从通道接收值 / Receive value from channel
    // 返回 std::nullopt 如果通道已关闭且为空
    std::optional<T> receive() {
        std::unique_lock<std::mutex> lock(mutex_);

        // 等待直到有数据或关闭
        notEmpty_.wait(lock, [this] {
            return closed_ || !queue_.empty();
        });

        if (queue_.empty()) return std::nullopt;

        T value = std::move(queue_.front());
        queue_.pop();
        notFull_.notify_one();
        return value;
    }

    // 非阻塞尝试接收 / Non-blocking try receive
    std::optional<T> tryReceive() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.empty()) return std::nullopt;

        T value = std::move(queue_.front());
        queue_.pop();
        notFull_.notify_one();
        return value;
    }

    // 检查是否有数据可接收 / Check if data is available to receive
    bool hasData() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !queue_.empty();
    }

    // 检查通道是否已关闭 / Check if channel is closed
    bool isClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    // 关闭通道 / Close channel
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    // 检查通道是否已关闭 / Check if channel is closed
    bool isClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    // 获取当前缓冲区大小 / Get current buffer size
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // 获取容量 (0 = 无界) / Get capacity (0 = unbounded)
    size_t capacity() const { return capacity_; }

private:
    size_t capacity_;
    Backpressure backpressure_;
    bool closed_;
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
};

} // namespace suki::runtime

// C 兼容的 channel 就绪检查函数 / C-compatible channel readiness check
extern "C" inline bool suki_channel_has_data(void* channel) {
    if (!channel) return false;
    // 简化实现：检查指针非空即认为就绪
    // Simplified: consider ready if pointer is non-null
    // 完整实现需要通过运行时类型信息检查 channel 内部状态
    return channel != nullptr;
}
