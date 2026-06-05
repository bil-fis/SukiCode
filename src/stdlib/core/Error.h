#pragma once
// SukiCode Error 协议和 Result 类型
// Error protocol and Result type for error handling.

#include <string>
#include <memory>
#include <functional>

namespace suki::stdlib {

// Error 协议基类 / Error protocol base class
// 所有错误类型必须继承此类 / All error types must inherit from this
class Error {
public:
    virtual ~Error() = default;

    // 获取错误描述 / Get error description
    virtual std::string description() const {
        return "Unknown error";
    }

    // 获取本地化描述 / Get localized description
    virtual std::string localizedDescription() const {
        return description();
    }
};

// 通用错误类型 / Generic error type
class GenericError : public Error {
public:
    explicit GenericError(std::string message)
        : message_(std::move(message)) {}

    std::string description() const override {
        return message_;
    }

private:
    std::string message_;
};

// Result<T, E> 类型 / Result type
// 用于不抛出异常的错误处理 / Used for non-throwing error handling
template<typename T, typename E = Error>
class Result {
public:
    // 成功构造 / Success constructor
    static Result success(T value) {
        Result r;
        r.success_ = true;
        r.value_ = std::move(value);
        return r;
    }

    // 失败构造 / Failure constructor
    static Result failure(E error) {
        Result r;
        r.success_ = false;
        r.error_ = std::move(error);
        return r;
    }

    // 是否成功 / Is success
    bool isSuccess() const { return success_; }

    // 是否失败 / Is failure
    bool isFailure() const { return !success_; }

    // 获取值（成功时）/ Get value (when success)
    const T& value() const { return value_; }
    T& value() { return value_; }

    // 获取错误（失败时）/ Get error (when failure)
    const E& error() const { return error_; }
    E& error() { return error_; }

    // 获取值或默认值 / Get value or default
    T valueOr(T defaultValue) const {
        return success_ ? value_ : std::move(defaultValue);
    }

    // 映射值 / Map value
    template<typename U>
    Result<U, E> map(std::function<U(const T&)> func) const {
        if (success_) {
            return Result<U, E>::success(func(value_));
        }
        return Result<U, E>::failure(error_);
    }

    // 映射错误 / Map error
    template<typename F>
    Result<T, F> mapError(std::function<F(const E&)> func) const {
        if (success_) {
            return Result<T, F>::success(value_);
        }
        return Result<T, F>::failure(func(error_));
    }

    // 链式操作 / Flat map
    template<typename U>
    Result<U, E> flatMap(std::function<Result<U, E>(const T&)> func) const {
        if (success_) {
            return func(value_);
        }
        return Result<U, E>::failure(error_);
    }

private:
    Result() : success_(false) {}

    bool success_;
    T value_;
    E error_;
};

// 便捷类型别名 / Convenience type aliases
using VoidResult = Result<std::nullptr_t, Error>;

} // namespace suki::stdlib
