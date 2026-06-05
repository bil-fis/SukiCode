#pragma once
// SukiCode Optional<T> 可选类型
// Optional type for nullable values.

#include <optional>
#include <functional>
#include <stdexcept>

namespace suki::stdlib {

// nil 表示 / nil representation
struct NilType {};
constexpr NilType nil{};

template<typename T>
class Optional {
public:
    // 构造函数 / Constructors
    Optional() : data_(std::nullopt) {}
    Optional(NilType) : data_(std::nullopt) {}
    Optional(const T& value) : data_(value) {}
    Optional(T&& value) : data_(std::move(value)) {}

    // 拷贝/移动 / Copy/Move
    Optional(const Optional&) = default;
    Optional(Optional&&) = default;
    Optional& operator=(const Optional&) = default;
    Optional& operator=(Optional&&) = default;

    // nil 赋值 / nil assignment
    Optional& operator=(NilType) { data_ = std::nullopt; return *this; }

    // 检查是否有值 / Check if has value
    bool hasValue() const { return data_.has_value(); }
    explicit operator bool() const { return hasValue(); }

    // 获取值 / Get value
    const T& value() const {
        if (!hasValue()) throw std::runtime_error("Optional has no value");
        return data_.value();
    }

    T& value() {
        if (!hasValue()) throw std::runtime_error("Optional has no value");
        return data_.value();
    }

    // 获取值或默认值 / Get value or default
    const T& valueOr(const T& defaultValue) const {
        return hasValue() ? data_.value() : defaultValue;
    }

    // 强制解包 / Force unwrap (operator !)
    const T& operator!() const { return value(); }

    // 可选链 / Optional chaining
    template<typename U>
    auto map(std::function<U(const T&)> func) const -> Optional<U> {
        if (hasValue()) return Optional<U>(func(data_.value()));
        return Optional<U>();
    }

    template<typename U>
    auto flatMap(std::function<Optional<U>(const T&)> func) const -> Optional<U> {
        if (hasValue()) return func(data_.value());
        return Optional<U>();
    }

    // 比较 / Comparison
    bool operator==(const Optional& other) const { return data_ == other.data_; }
    bool operator!=(const Optional& other) const { return data_ != other.data_; }
    bool operator==(NilType) const { return !hasValue(); }
    bool operator!=(NilType) const { return hasValue(); }
    bool operator==(const T& value) const { return hasValue() && data_.value() == value; }

private:
    std::optional<T> data_;
};

} // namespace suki::stdlib
