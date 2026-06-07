#pragma once
// SukiCode 错误栈 - 支持嵌套 throw
// Error stack for nested throw support.

#include <vector>
#include <mutex>

namespace suki::runtime {

// 线程局部错误栈 / Thread-local error stack
class ErrorStack {
public:
    static ErrorStack& instance() {
        static thread_local ErrorStack stack;
        return stack;
    }

    void push(void* error) {
        errors_.push_back(error);
    }

    void* pop() {
        if (errors_.empty()) return nullptr;
        void* error = errors_.back();
        errors_.pop_back();
        return error;
    }

    void* peek() const {
        if (errors_.empty()) return nullptr;
        return errors_.back();
    }

    bool isEmpty() const {
        return errors_.empty();
    }

    size_t size() const {
        return errors_.size();
    }

private:
    std::vector<void*> errors_;
};

} // namespace suki::runtime

// C 兼容接口 / C-compatible interface
extern "C" inline void suki_push_error(void* error) {
    suki::runtime::ErrorStack::instance().push(error);
}

extern "C" inline void* suki_pop_error() {
    return suki::runtime::ErrorStack::instance().pop();
}

extern "C" inline void* suki_peek_error() {
    return suki::runtime::ErrorStack::instance().peek();
}

extern "C" inline int suki_has_error() {
    return suki::runtime::ErrorStack::instance().isEmpty() ? 0 : 1;
}
