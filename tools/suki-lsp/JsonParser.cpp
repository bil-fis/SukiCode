// JSON parser implementation for LSP messages.

#include "JsonParser.h"
#include <sstream>
#include <stdexcept>

namespace suki::lsp {

// ─── JsonValue serialization ────────────────────────────────────────────

std::string JsonValue::dump() const {
    std::ostringstream oss;
    switch (type_) {
        case Type::Null: oss << "null"; break;
        case Type::Bool: oss << (boolVal_ ? "true" : "false"); break;
        case Type::Number:
            if (numVal_ == static_cast<int64_t>(numVal_))
                oss << static_cast<int64_t>(numVal_);
            else
                oss << numVal_;
            break;
        case Type::String:
            oss << '"';
            for (char c : strVal_) {
                if (c == '"') oss << "\\\"";
                else if (c == '\\') oss << "\\\\";
                else if (c == '\n') oss << "\\n";
                else if (c == '\t') oss << "\\t";
                else oss << c;
            }
            oss << '"';
            break;
        case Type::Array:
            oss << '[';
            for (size_t i = 0; i < arrVal_.size(); i++) {
                if (i > 0) oss << ',';
                oss << arrVal_[i].dump();
            }
            oss << ']';
            break;
        case Type::Object:
            oss << '{';
            bool first = true;
            for (const auto& [k, v] : objVal_) {
                if (!first) oss << ',';
                oss << '"' << k << "\":" << v.dump();
                first = false;
            }
            oss << '}';
            break;
    }
    return oss.str();
}

// ─── JsonParser ─────────────────────────────────────────────────────────

JsonValue JsonParser::parse(const std::string& str) {
    size_t pos = 0;
    skipWhitespace(str, pos);
    return parseValue(str, pos);
}

JsonValue JsonParser::parseValue(const std::string& str, size_t& pos) {
    skipWhitespace(str, pos);
    if (pos >= str.size()) return JsonValue();

    char c = str[pos];
    if (c == '"') return parseString(str, pos);
    if (c == '{') return parseObject(str, pos);
    if (c == '[') return parseArray(str, pos);
    if (c == 't' || c == 'f') return parseBool(str, pos);
    if (c == 'n') return parseNull(str, pos);
    if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(str, pos);
    return JsonValue();
}

JsonValue JsonParser::parseString(const std::string& str, size_t& pos) {
    pos++; // skip opening "
    std::string value;
    while (pos < str.size() && str[pos] != '"') {
        if (str[pos] == '\\' && pos + 1 < str.size()) {
            pos++;
            switch (str[pos]) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case '\\': value += '\\'; break;
                case '"': value += '"'; break;
                default: value += str[pos]; break;
            }
        } else {
            value += str[pos];
        }
        pos++;
    }
    if (pos < str.size()) pos++; // skip closing "
    return JsonValue(value);
}

JsonValue JsonParser::parseNumber(const std::string& str, size_t& pos) {
    size_t start = pos;
    if (str[pos] == '-') pos++;
    while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9') pos++;
    bool isFloat = false;
    if (pos < str.size() && str[pos] == '.') {
        isFloat = true;
        pos++;
        while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9') pos++;
    }
    if (pos < str.size() && (str[pos] == 'e' || str[pos] == 'E')) {
        isFloat = true;
        pos++;
        if (pos < str.size() && (str[pos] == '+' || str[pos] == '-')) pos++;
        while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9') pos++;
    }
    std::string numStr = str.substr(start, pos - start);
    if (isFloat) return JsonValue(std::stod(numStr));
    return JsonValue(static_cast<int>(std::stoll(numStr)));
}

JsonValue JsonParser::parseBool(const std::string& str, size_t& pos) {
    if (str.substr(pos, 4) == "true") { pos += 4; return JsonValue(true); }
    if (str.substr(pos, 5) == "false") { pos += 5; return JsonValue(false); }
    return JsonValue();
}

JsonValue JsonParser::parseNull(const std::string& str, size_t& pos) {
    if (str.substr(pos, 4) == "null") { pos += 4; return JsonValue(); }
    return JsonValue();
}

JsonValue JsonParser::parseArray(const std::string& str, size_t& pos) {
    pos++; // skip [
    JsonValue::ArrayType arr;
    skipWhitespace(str, pos);
    while (pos < str.size() && str[pos] != ']') {
        arr.push_back(parseValue(str, pos));
        skipWhitespace(str, pos);
        if (pos < str.size() && str[pos] == ',') pos++;
        skipWhitespace(str, pos);
    }
    if (pos < str.size()) pos++; // skip ]
    return JsonValue(arr);
}

JsonValue JsonParser::parseObject(const std::string& str, size_t& pos) {
    pos++; // skip {
    JsonValue::ObjectType obj;
    skipWhitespace(str, pos);
    while (pos < str.size() && str[pos] != '}') {
        skipWhitespace(str, pos);
        JsonValue key = parseString(str, pos);
        skipWhitespace(str, pos);
        if (pos < str.size() && str[pos] == ':') pos++;
        skipWhitespace(str, pos);
        JsonValue value = parseValue(str, pos);
        obj[key.stringValue()] = value;
        skipWhitespace(str, pos);
        if (pos < str.size() && str[pos] == ',') pos++;
        skipWhitespace(str, pos);
    }
    if (pos < str.size()) pos++; // skip }
    return JsonValue(obj);
}

void JsonParser::skipWhitespace(const std::string& str, size_t& pos) {
    while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t' ||
           str[pos] == '\n' || str[pos] == '\r')) pos++;
}

} // namespace suki::lsp
