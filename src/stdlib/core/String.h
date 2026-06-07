#pragma once
// SukiCode String 类型实现
// UTF-8 string with COW (Copy-On-Write) optimization.

#include <string>
#include <string_view>
#include <memory>
#include <cstdint>
#include <functional>

namespace suki::stdlib {

// Unicode 标量值 / Unicode scalar value (21-bit)
using UnicodeScalar = char32_t;

// String 类型 / String type
// 值类型，内部使用 COW 优化 / Value type with COW optimization
class String {
public:
    // 构造函数 / Constructors
    String() : data_(std::make_shared<std::string>()) {}
    String(const char* str) : data_(std::make_shared<std::string>(str)) {}
    String(const std::string& str) : data_(std::make_shared<std::string>(str)) {}
    String(std::string&& str) : data_(std::make_shared<std::string>(std::move(str))) {}
    String(std::string_view str) : data_(std::make_shared<std::string>(str)) {}

    // 拷贝构造（COW - 共享数据）/ Copy constructor (COW - share data)
    String(const String& other) : data_(other.data_) {}

    // 移动构造 / Move constructor
    String(String&& other) noexcept : data_(std::move(other.data_)) {}

    // 赋值操作符 / Assignment operators
    String& operator=(const String& other) {
        data_ = other.data_;
        return *this;
    }
    String& operator=(String&& other) noexcept {
        data_ = std::move(other.data_);
        return *this;
    }

    // 获取长度（字节数）/ Get length (byte count)
    size_t length() const { return data_->size(); }

    // 获取字符数（Unicode 标量）/ Get character count (Unicode scalars)
    size_t count() const;

    // 是否为空 / Is empty
    bool isEmpty() const { return data_->empty(); }

    // 获取 C 字符串 / Get C string
    const char* cStr() const { return data_->c_str(); }

    // 获取 string_view
    std::string_view view() const { return *data_; }

    // 获取底层 string（可能触发 COW）/ Get underlying string (may trigger COW)
    std::string& mutableRef() {
        ensureUnique();
        return *data_;
    }

    // 字符访问 / Character access
    char operator[](size_t index) const { return (*data_)[index]; }

    // 连接 / Concatenation
    String operator+(const String& other) const {
        return String(*data_ + *other.data_);
    }
    String operator+(const char* str) const {
        return String(*data_ + str);
    }
    String& operator+=(const String& other) {
        ensureUnique();
        *data_ += *other.data_;
        return *this;
    }

    // 比较 / Comparison
    bool operator==(const String& other) const { return *data_ == *other.data_; }
    bool operator!=(const String& other) const { return *data_ != *other.data_; }
    bool operator<(const String& other) const { return *data_ < *other.data_; }
    bool operator>(const String& other) const { return *data_ > *other.data_; }

    // 子字符串 / Substring
    String substring(size_t start, size_t count = std::string::npos) const {
        return String(data_->substr(start, count));
    }

    // 查找 / Find
    size_t find(const String& str, size_t pos = 0) const {
        return data_->find(*str.data_, pos);
    }
    size_t find(char ch, size_t pos = 0) const {
        return data_->find(ch, pos);
    }

    // 包含 / Contains
    bool contains(const String& str) const {
        return find(str) != std::string::npos;
    }

    // 前缀/后缀 / Prefix/suffix
    bool hasPrefix(const String& prefix) const {
        if (prefix.length() > length()) return false;
        return data_->compare(0, prefix.length(), *prefix.data_) == 0;
    }
    bool hasSuffix(const String& suffix) const {
        if (suffix.length() > length()) return false;
        return data_->compare(length() - suffix.length(), suffix.length(), *suffix.data_) == 0;
    }

    // 大小写转换 / Case conversion
    String uppercased() const;
    String lowercased() const;

    // 去除空白 / Trim
    String trimmingWhitespace() const;

    // 分割 / Split
    std::vector<String> split(char separator) const;

    // 替换 / Replace
    String replacing(const String& target, const String& replacement) const;

    // UTF-8 视图 / UTF-8 view
    const char* utf8() const { return data_->c_str(); }
    size_t utf8Length() const { return data_->size(); }

    // UTF-16 视图 / UTF-16 view
    std::vector<char16_t> utf16() const;

    // 哈希 / Hash
    size_t hash() const {
        return std::hash<std::string>{}(*data_);
    }

    // Unicode 视图 / Unicode views
    std::vector<UnicodeScalar> unicodeScalars() const;

    // 字符视图（同 unicodeScalars，因为 Char = UnicodeScalar）/ Characters view
    std::vector<UnicodeScalar> characters() const { return unicodeScalars(); }

private:
    // 确保数据唯一（COW）/ Ensure data is unique (COW)
    void ensureUnique() {
        if (!data_ || data_.use_count() > 1) {
            data_ = std::make_shared<std::string>(*data_);
        }
    }

    std::shared_ptr<std::string> data_;
};

// 字符串字面量操作符 / String literal operator
inline String operator""_s(const char* str, size_t len) {
    return String(std::string_view(str, len));
}

} // namespace suki::stdlib

// 哈希特化 / Hash specialization
namespace std {
template<>
struct hash<suki::stdlib::String> {
    size_t operator()(const suki::stdlib::String& s) const {
        return s.hash();
    }
};
}
