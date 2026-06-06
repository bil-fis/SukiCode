#pragma once
// SukiCode JSON 编解码器
// JSON encoder/decoder.

#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <sstream>
#include <stdexcept>

namespace suki::stdlib {

// JSON 值类型 / JSON value type
class JSON {
public:
    // JSON 类型 / JSON types
    enum class Type { Null, Bool, Number, String, Array, Object };

    using ArrayType = std::vector<JSON>;
    using ObjectType = std::unordered_map<std::string, JSON>;

    // 构造函数 / Constructors
    JSON() : type_(Type::Null) {}
    JSON(bool value) : type_(Type::Bool), boolValue_(value) {}
    JSON(int value) : type_(Type::Number), numberValue_(value) {}
    JSON(double value) : type_(Type::Number), numberValue_(value) {}
    JSON(const std::string& value) : type_(Type::String), stringValue_(value) {}
    JSON(const char* value) : type_(Type::String), stringValue_(value) {}
    JSON(const ArrayType& value) : type_(Type::Array), arrayValue_(value) {}
    JSON(const ObjectType& value) : type_(Type::Object), objectValue_(value) {}

    // 类型检查 / Type checks
    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // 获取值 / Get values
    bool boolValue() const { return boolValue_; }
    double numberValue() const { return numberValue_; }
    int intValue() const { return static_cast<int>(numberValue_); }
    const std::string& stringValue() const { return stringValue_; }
    const ArrayType& arrayValue() const { return arrayValue_; }
    const ObjectType& objectValue() const { return objectValue_; }

    // 数组操作 / Array operations
    void push(const JSON& value) { arrayValue_.push_back(value); }
    size_t size() const {
        if (type_ == Type::Array) return arrayValue_.size();
        if (type_ == Type::Object) return objectValue_.size();
        return 0;
    }

    // 对象操作 / Object operations
    JSON& operator[](const std::string& key) { return objectValue_[key]; }
    const JSON& operator[](const std::string& key) const {
        auto it = objectValue_.find(key);
        if (it == objectValue_.end()) throw std::runtime_error("key not found: " + key);
        return it->second;
    }
    bool contains(const std::string& key) const {
        return objectValue_.count(key) > 0;
    }

    // 序列化 / Serialization
    std::string dump(int indent = 0) const {
        std::ostringstream oss;
        dumpTo(oss, indent, 0);
        return oss.str();
    }

    // 解析 / Parsing
    static JSON parse(const std::string& str) {
        size_t pos = 0;
        return parseValue(str, pos);
    }

private:
    void dumpTo(std::ostringstream& oss, int indent, int depth) const {
        std::string pad(indent * depth, ' ');
        std::string padInner(indent * (depth + 1), ' ');

        switch (type_) {
            case Type::Null:
                oss << "null";
                break;
            case Type::Bool:
                oss << (boolValue_ ? "true" : "false");
                break;
            case Type::Number:
                if (numberValue_ == static_cast<int64_t>(numberValue_)) {
                    oss << static_cast<int64_t>(numberValue_);
                } else {
                    oss << numberValue_;
                }
                break;
            case Type::String:
                oss << "\"";
                for (unsigned char c : stringValue_) {
                    if (c == '"') oss << "\\\"";
                    else if (c == '\\') oss << "\\\\";
                    else if (c == '\n') oss << "\\n";
                    else if (c == '\t') oss << "\\t";
                    else if (c == '\r') oss << "\\r";
                    else if (c == '\b') oss << "\\b";
                    else if (c == '\f') oss << "\\f";
                    else if (c < 0x20) {
                        // Control characters: \u00XX
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        oss << buf;
                    }
                    else oss << c;
                }
                oss << "\"";
                break;
            case Type::Array:
                oss << "[";
                if (indent > 0) oss << "\n";
                for (size_t i = 0; i < arrayValue_.size(); i++) {
                    if (i > 0) oss << ",";
                    if (indent > 0) oss << "\n" << padInner;
                    arrayValue_[i].dumpTo(oss, indent, depth + 1);
                }
                if (indent > 0) oss << "\n" << pad;
                oss << "]";
                break;
            case Type::Object:
                oss << "{";
                if (indent > 0) oss << "\n";
                bool first = true;
                for (const auto& [key, val] : objectValue_) {
                    if (!first) oss << ",";
                    if (indent > 0) oss << "\n" << padInner;
                    oss << "\"" << key << "\":";
                    if (indent > 0) oss << " ";
                    val.dumpTo(oss, indent, depth + 1);
                    first = false;
                }
                if (indent > 0) oss << "\n" << pad;
                oss << "}";
                break;
        }
    }

    static JSON parseValue(const std::string& str, size_t& pos) {
        skipWhitespace(str, pos);
        if (pos >= str.size()) return JSON();

        char c = str[pos];
        if (c == '"') return parseString(str, pos);
        if (c == '{') return parseObject(str, pos);
        if (c == '[') return parseArray(str, pos);
        if (c == 't' || c == 'f') return parseBool(str, pos);
        if (c == 'n') return parseNull(str, pos);
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(str, pos);
        throw std::runtime_error("unexpected character in JSON");
    }

    static void skipWhitespace(const std::string& str, size_t& pos) {
        while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t' ||
               str[pos] == '\n' || str[pos] == '\r')) pos++;
    }

    static JSON parseString(const std::string& str, size_t& pos) {
        pos++; // skip "
        std::string value;
        while (pos < str.size() && str[pos] != '"') {
            if (str[pos] == '\\') {
                pos++;
                if (pos < str.size()) {
                    switch (str[pos]) {
                        case 'n': value += '\n'; break;
                        case 't': value += '\t'; break;
                        case 'r': value += '\r'; break;
                        case 'b': value += '\b'; break;
                        case 'f': value += '\f'; break;
                        case '\\': value += '\\'; break;
                        case '"': value += '"'; break;
                        case '/': value += '/'; break;
                        case 'u': {
                            // Unicode escape: \uXXXX
                            if (pos + 4 < str.size()) {
                                uint32_t codepoint = 0;
                                for (int i = 1; i <= 4; i++) {
                                    char c = str[pos + i];
                                    codepoint <<= 4;
                                    if (c >= '0' && c <= '9') codepoint += c - '0';
                                    else if (c >= 'a' && c <= 'f') codepoint += c - 'a' + 10;
                                    else if (c >= 'A' && c <= 'F') codepoint += c - 'A' + 10;
                                }
                                pos += 4;
                                // UTF-8 encoding
                                if (codepoint < 0x80) {
                                    value += static_cast<char>(codepoint);
                                } else if (codepoint < 0x800) {
                                    value += static_cast<char>(0xC0 | (codepoint >> 6));
                                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                                } else if (codepoint < 0x10000) {
                                    value += static_cast<char>(0xE0 | (codepoint >> 12));
                                    value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                                } else {
                                    value += static_cast<char>(0xF0 | (codepoint >> 18));
                                    value += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                                    value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                                }
                            }
                            break;
                        }
                        default: value += str[pos]; break;
                    }
                }
            } else {
                value += str[pos];
            }
            pos++;
        }
        pos++; // skip closing "
        return JSON(value);
    }

    static JSON parseNumber(const std::string& str, size_t& pos) {
        size_t start = pos;
        // 符号 / Sign
        if (pos < str.size() && str[pos] == '-') pos++;
        // 整数部分 / Integer part
        while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9') pos++;
        // 小数部分 / Fractional part
        if (pos < str.size() && str[pos] == '.') {
            pos++;
            while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9') pos++;
        }
        // 指数部分 / Exponent part (e/E with optional +/-)
        if (pos < str.size() && (str[pos] == 'e' || str[pos] == 'E')) {
            pos++;
            if (pos < str.size() && (str[pos] == '+' || str[pos] == '-')) pos++;
            while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9') pos++;
        }
        return JSON(std::stod(str.substr(start, pos - start)));
    }

    static JSON parseBool(const std::string& str, size_t& pos) {
        if (str.substr(pos, 4) == "true") { pos += 4; return JSON(true); }
        if (str.substr(pos, 5) == "false") { pos += 5; return JSON(false); }
        throw std::runtime_error("invalid boolean");
    }

    static JSON parseNull(const std::string& str, size_t& pos) {
        if (str.substr(pos, 4) == "null") { pos += 4; return JSON(); }
        throw std::runtime_error("invalid null");
    }

    static JSON parseArray(const std::string& str, size_t& pos) {
        pos++; // skip [
        ArrayType arr;
        skipWhitespace(str, pos);
        while (pos < str.size() && str[pos] != ']') {
            arr.push_back(parseValue(str, pos));
            skipWhitespace(str, pos);
            if (pos < str.size() && str[pos] == ',') pos++;
            skipWhitespace(str, pos);
        }
        pos++; // skip ]
        return JSON(arr);
    }

    static JSON parseObject(const std::string& str, size_t& pos) {
        pos++; // skip {
        ObjectType obj;
        skipWhitespace(str, pos);
        while (pos < str.size() && str[pos] != '}') {
            skipWhitespace(str, pos);
            JSON key = parseString(str, pos);
            skipWhitespace(str, pos);
            if (pos < str.size() && str[pos] == ':') pos++;
            skipWhitespace(str, pos);
            JSON value = parseValue(str, pos);
            obj[key.stringValue()] = value;
            skipWhitespace(str, pos);
            if (pos < str.size() && str[pos] == ',') pos++;
            skipWhitespace(str, pos);
        }
        pos++; // skip }
        return JSON(obj);
    }

    Type type_;
    bool boolValue_ = false;
    double numberValue_ = 0;
    std::string stringValue_;
    ArrayType arrayValue_;
    ObjectType objectValue_;
};

} // namespace suki::stdlib
