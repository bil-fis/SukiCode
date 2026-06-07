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

// Channel 基类，存储就绪检查回调 / Channel base class, stores readiness check callback
struct ChannelBase {
    using HasDataFunc = int(*)(void*, void*);
    HasDataFunc hasDataFunc_ = nullptr;
    void* hasDataSelf_ = nullptr;
};

template<typename T>
class Channel : public ChannelBase {
public:
    // 创建有界通道 / Create bounded channel
    explicit Channel(size_t capacity, Backpressure bp = Backpressure::Block)
        : capacity_(capacity), backpressure_(bp), closed_(false) {
        registerHasDataFunc();
    }

    // 创建无界通道 / Create unbounded channel
    Channel() : capacity_(0), backpressure_(Backpressure::Block), closed_(false) {
        registerHasDataFunc();
    }

    ~Channel() { close(); }

    // 注册 hasData 函数指针供 select 代码生成使用
    // Register hasData function pointer for select codegen
    void registerHasDataFunc() {
        // 存储 this 指针和静态回调 / Store this pointer and static callback
        hasDataSelf_ = this;
        hasDataFunc_ = &Channel::staticHasData;
    }

    // 静态回调函数 / Static callback function
    static int staticHasData(void* self, void*) {
        if (!self) return 0;
        Channel* ch = static_cast<Channel*>(self);
        return ch->hasData() ? 1 : 0;
    }

    // hasDataFunc_ 和 hasDataSelf_ 继承自 ChannelBase

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

    // C 兼容的就绪检查 / C-compatible readiness check
    int hasDataC() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty() ? 0 : 1;
    }

    // 实例级就绪检查回调 / Instance-level readiness check callback
    // 存储类型擦除的检查函数
    using HasDataFunc = int(*)(void*, void*);
    HasDataFunc hasDataFunc_ = nullptr;
    void* hasDataCtx_ = nullptr;

    // 设置就绪检查 / Set readiness check
    void setHasDataFunc(HasDataFunc func, void* ctx) {
        hasDataFunc_ = func;
        hasDataCtx_ = ctx;
    }

    // 调用就绪检查 / Invoke readiness check
    int invokeHasDataCheck() const {
        if (hasDataFunc_) return hasDataFunc_(hasDataCtx_, nullptr);
        return hasData() ? 1 : 0;
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
// 通过 ChannelBase 基类的 hasDataFunc_ 回调检查就绪状态
// Uses hasDataFunc_ callback from ChannelBase to check readiness
extern "C" inline bool suki_channel_has_data(void* channelPtr) {
    if (!channelPtr) return false;
    // ChannelBase 是 Channel 的基类，hasDataFunc_ 在固定偏移处
    // ChannelBase is base class of Channel, hasDataFunc_ at fixed offset
    ChannelBase* base = static_cast<ChannelBase*>(channelPtr);
    if (base->hasDataFunc_) {
        return base->hasDataFunc_(base->hasDataSelf_, nullptr) != 0;
    }
    // 回退：检查指针有效性 / Fallback: check pointer validity
    return channelPtr != nullptr;
}
