#pragma once
// SukiCode Queue<T> - 先进先出队列
// First-in-first-out queue.

#include <deque>
#include <cstddef>
#include <stdexcept>

namespace suki::stdlib {

template<typename T>
class Queue {
public:
    Queue() = default;

    // 入队 / Enqueue
    void enqueue(const T& value) { data_.push_back(value); }
    void enqueue(T&& value) { data_.push_back(std::move(value)); }

    // 出队 / Dequeue
    T dequeue() {
        if (isEmpty()) throw std::runtime_error("queue is empty");
        T value = std::move(data_.front());
        data_.pop_front();
        return value;
    }

    // 查看队首 / Peek front
    const T& front() const {
        if (isEmpty()) throw std::runtime_error("queue is empty");
        return data_.front();
    }

    // 大小 / Size
    size_t count() const { return data_.size(); }
    bool isEmpty() const { return data_.empty(); }

    // 清空 / Clear
    void clear() { data_.clear(); }

private:
    std::deque<T> data_;
};

} // namespace suki::stdlib
