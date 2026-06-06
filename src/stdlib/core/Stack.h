#pragma once
// SukiCode Stack<T> - 后进先出栈
// Last-in-first-out stack.

#include <vector>
#include <cstddef>
#include <stdexcept>

namespace suki::stdlib {

template<typename T>
class Stack {
public:
    Stack() = default;

    // 压栈 / Push
    void push(const T& value) { data_.push_back(value); }
    void push(T&& value) { data_.push_back(std::move(value)); }

    // 弹栈 / Pop
    T pop() {
        if (isEmpty()) throw std::runtime_error("stack is empty");
        T value = std::move(data_.back());
        data_.pop_back();
        return value;
    }

    // 查看栈顶 / Peek
    const T& peek() const {
        if (isEmpty()) throw std::runtime_error("stack is empty");
        return data_.back();
    }

    // 大小 / Size
    size_t count() const { return data_.size(); }
    bool isEmpty() const { return data_.empty(); }

    // 清空 / Clear
    void clear() { data_.clear(); }

private:
    std::vector<T> data_;
};

} // namespace suki::stdlib
