#pragma once
// 轻量级 JSON 解析器 - 用于 LSP 消息解析
// Lightweight JSON parser for LSP message parsing.

#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <cstdint>

namespace suki::lsp {

// JSON 值类型 / JSON value type
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using ArrayType = std::vector<JsonValue>;
    using ObjectType = std::unordered_map<std::string, JsonValue>;

    JsonValue() : type_(Type::Null) {}
    JsonValue(bool v) : type_(Type::Bool), boolVal_(v) {}
    JsonValue(int v) : type_(Type::Number), numVal_(v) {}
    JsonValue(double v) : type_(Type::Number), numVal_(v) {}
    JsonValue(const std::string& v) : type_(Type::String), strVal_(v) {}
    JsonValue(const char* v) : type_(Type::String), strVal_(v) {}
    JsonValue(const ArrayType& v) : type_(Type::Array), arrVal_(v) {}
    JsonValue(const ObjectType& v) : type_(Type::Object), objVal_(v) {}

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool boolValue() const { return boolVal_; }
    double numberValue() const { return numVal_; }
    int intValue() const { return static_cast<int>(numVal_); }
    const std::string& stringValue() const { return strVal_; }
    const ArrayType& arrayValue() const { return arrVal_; }
    const ObjectType& objectValue() const { return objVal_; }

    // 对象访问 / Object access
    bool hasKey(const std::string& key) const {
        return type_ == Type::Object && objVal_.count(key) > 0;
    }

    const JsonValue& operator[](const std::string& key) const {
        static JsonValue null;
        if (type_ != Type::Object) return null;
        auto it = objVal_.find(key);
        return it != objVal_.end() ? it->second : null;
    }

    // 数组访问 / Array access
    const JsonValue& operator[](size_t index) const {
        static JsonValue null;
        if (type_ != Type::Array || index >= arrVal_.size()) return null;
        return arrVal_[index];
    }

    size_t size() const {
        if (type_ == Type::Array) return arrVal_.size();
        if (type_ == Type::Object) return objVal_.size();
        return 0;
    }

    // 序列化 / Serialization
    std::string dump() const;

private:
    Type type_;
    bool boolVal_ = false;
    double numVal_ = 0;
    std::string strVal_;
    ArrayType arrVal_;
    ObjectType objVal_;
};

// JSON 解析器 / JSON parser
class JsonParser {
public:
    static JsonValue parse(const std::string& str);

private:
    static JsonValue parseValue(const std::string& str, size_t& pos);
    static JsonValue parseString(const std::string& str, size_t& pos);
    static JsonValue parseNumber(const std::string& str, size_t& pos);
    static JsonValue parseBool(const std::string& str, size_t& pos);
    static JsonValue parseNull(const std::string& str, size_t& pos);
    static JsonValue parseArray(const std::string& str, size_t& pos);
    static JsonValue parseObject(const std::string& str, size_t& pos);
    static void skipWhitespace(const std::string& str, size_t& pos);
};

} // namespace suki::lsp
