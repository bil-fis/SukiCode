#pragma once
// SukiCode Atomic<T> 运行时实现
// Thread-safe atomic operations for integer and pointer types.

#include <atomic>
#include <cstdint>

namespace suki::runtime {

// 原子整数类型 / Atomic integer type
template<typename T>
class Atomic {
public:
    Atomic() : value_(T{}) {}
    Atomic(T value) : value_(value) {}
    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

    // 加载值 / Load value
    T load(std::memory_order order = std::memory_order_seq_cst) const {
        return value_.load(order);
    }

    // 存储值 / Store value
    void store(T value, std::memory_order order = std::memory_order_seq_cst) {
        value_.store(value, order);
    }

    // 交换值 / Exchange value
    T exchange(T value, std::memory_order order = std::memory_order_seq_cst) {
        return value_.exchange(value, order);
    }

    // 比较并交换 / Compare and swap
    bool compareAndSwap(T& expected, T desired,
                        std::memory_order success = std::memory_order_seq_cst,
                        std::memory_order failure = std::memory_order_seq_cst) {
        return value_.compare_exchange_strong(expected, desired, success, failure);
    }

    // 原子加 / Atomic add
    T fetchAdd(T value, std::memory_order order = std::memory_order_seq_cst) {
        return value_.fetch_add(value, order);
    }

    // 原子减 / Atomic subtract
    T fetchSub(T value, std::memory_order order = std::memory_order_seq_cst) {
        return value_.fetch_sub(value, order);
    }

    // 原子与 / Atomic AND
    T fetchAnd(T value, std::memory_order order = std::memory_order_seq_cst) {
        return value_.fetch_and(value, order);
    }

    // 原子或 / Atomic OR
    T fetchOr(T value, std::memory_order order = std::memory_order_seq_cst) {
        return value_.fetch_or(value, order);
    }

    // 原子异或 / Atomic XOR
    T fetchXor(T value, std::memory_order order = std::memory_order_seq_cst) {
        return value_.fetch_xor(value, order);
    }

    // 前缀递增 / Prefix increment
    T operator++() { return value_.fetch_add(1) + 1; }

    // 后缀递增 / Postfix increment
    T operator++(int) { return value_.fetch_add(1); }

    // 前缀递减 / Prefix decrement
    T operator--() { return value_.fetch_sub(1) - 1; }

    // 后缀递减 / Postfix decrement
    T operator--(int) { return value_.fetch_sub(1); }

    // 赋值操作符 / Assignment operators
    T operator+=(T value) { return fetchAdd(value) + value; }
    T operator-=(T value) { return fetchSub(value) - value; }
    T operator&=(T value) { return fetchAnd(value) & value; }
    T operator|=(T value) { return fetchOr(value) | value; }
    T operator^=(T value) { return fetchXor(value) ^ value; }

    // 隐式转换 / Implicit conversion
    operator T() const { return load(); }

    // 赋值 / Assignment
    T operator=(T value) {
        store(value);
        return value;
    }

private:
    std::atomic<T> value_;
};

// 常用特化 / Common specializations
using AtomicInt = Atomic<int64_t>;
using AtomicUInt = Atomic<uint64_t>;
using AtomicBool = Atomic<bool>;

// 原子指针类型 / Atomic pointer type
template<typename T>
class AtomicPtr {
public:
    AtomicPtr() : ptr_(nullptr) {}
    AtomicPtr(T* ptr) : ptr_(ptr) {}

    T* load(std::memory_order order = std::memory_order_seq_cst) const {
        return ptr_.load(order);
    }

    void store(T* ptr, std::memory_order order = std::memory_order_seq_cst) {
        ptr_.store(ptr, order);
    }

    T* exchange(T* ptr, std::memory_order order = std::memory_order_seq_cst) {
        return ptr_.exchange(ptr, order);
    }

    bool compareAndSwap(T*& expected, T* desired,
                        std::memory_order success = std::memory_order_seq_cst,
                        std::memory_order failure = std::memory_order_seq_cst) {
        return ptr_.compare_exchange_strong(expected, desired, success, failure);
    }

    operator T*() const { return load(); }
    T* operator=(T* ptr) { store(ptr); return ptr; }

private:
    std::atomic<T*> ptr_;
};

} // namespace suki::runtime
