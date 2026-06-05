#pragma once
// SukiCode Range<T> 区间类型
// Half-open range [start, end) and closed range [start, end].

#include <cstddef>
#include <functional>

namespace suki::stdlib {

// 半开区间 / Half-open range: start..<end
template<typename T>
class Range {
public:
    Range(T start, T end) : start_(start), end_(end) {}

    T start() const { return start_; }
    T end() const { return end_; }

    bool contains(const T& value) const { return value >= start_ && value < end_; }
    bool isEmpty() const { return start_ >= end_; }

    // 迭代 / Iteration
    class Iterator {
    public:
        Iterator(T value) : value_(value) {}
        T operator*() const { return value_; }
        Iterator& operator++() { ++value_; return *this; }
        bool operator!=(const Iterator& other) const { return value_ != other.value_; }
    private:
        T value_;
    };

    Iterator begin() const { return Iterator(start_); }
    Iterator end() const { return Iterator(end_); }

private:
    T start_;
    T end_;
};

// 闭区间 / Closed range: start...end
template<typename T>
class ClosedRange {
public:
    ClosedRange(T start, T end) : start_(start), end_(end) {}

    T start() const { return start_; }
    T end() const { return end_; }

    bool contains(const T& value) const { return value >= start_ && value <= end_; }
    bool isEmpty() const { return start_ > end_; }

    // 迭代 / Iteration
    class Iterator {
    public:
        Iterator(T value) : value_(value) {}
        T operator*() const { return value_; }
        Iterator& operator++() { ++value_; return *this; }
        bool operator!=(const Iterator& other) const { return value_ != other.value_; }
    private:
        T value_;
    };

    Iterator begin() const { return Iterator(start_); }
    Iterator end() const { return Iterator(end_ + 1); }

private:
    T start_;
    T end_;
};

// 便捷函数 / Convenience functions
template<typename T>
Range<T> range(T start, T end) { return Range<T>(start, end); }

template<typename T>
ClosedRange<T> closedRange(T start, T end) { return ClosedRange<T>(start, end); }

// stride 函数 / stride function
template<typename T>
void stride(from: T, to: T, by: T, std::function<void(T)> body) {
    if (by > 0) {
        for (T i = from; i < to; i += by) body(i);
    } else if (by < 0) {
        for (T i = from; i > to; i += by) body(i);
    }
}

} // namespace suki::stdlib
