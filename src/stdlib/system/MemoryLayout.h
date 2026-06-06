#pragma once
// SukiCode MemoryLayout - 查询类型大小、对齐和步长
// Query type size, alignment, and stride.

#include <cstddef>
#include <type_traits>

namespace suki::stdlib {

// MemoryLayout<T> 查询类型的内存布局信息
// Query memory layout information for a type.
template<typename T>
struct MemoryLayout {
    // 类型的大小（字节）/ Size of the type in bytes
    static constexpr size_t size = sizeof(T);

    // 类型的对齐要求（字节）/ Alignment requirement in bytes
    static constexpr size_t alignment = alignof(T);

    // 数组中元素之间的步长（字节）/ Stride between elements in an array
    static constexpr size_t stride = sizeof(T);

    // 是否是空类型 / Whether the type is empty
    static constexpr bool isEmpty = std::is_empty_v<T>;

    // 是否是平凡类型（可 memcpy）/ Whether the type is trivially copyable
    static constexpr bool isTriviallyCopyable = std::is_trivially_copyable_v<T>;

    // 是否是 POD 类型 / Whether the type is POD
    static constexpr bool isPOD = std::is_standard_layout_v<T> && std::is_trivial_v<T>;
};

} // namespace suki::stdlib
