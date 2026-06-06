#pragma once
// SukiCode 序列化模块 - XML、CSV、MessagePack
// Serialization: XML, CSV, MessagePack.

#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <functional>
#include <cstdint>
#include <cstring>

namespace suki::data {

// ─── XML ────────────────────────────────────────────────────────────────

// XML 节点 / XML Node
class XMLNode {
public:
    std::string name;
    std::string text;
    std::unordered_map<std::string, std::string> attributes;
    std::vector<XMLNode> children;

    // 添加子节点 / Add child
    XMLNode& addChild(const std::string& childName) {
        children.push_back(XMLNode{childName});
        return children.back();
    }

    // 设置属性 / Set attribute
    void setAttribute(const std::string& key, const std::string& value) {
        attributes[key] = value;
    }

    // 获取属性 / Get attribute
    std::string attribute(const std::string& key, const std::string& defaultVal = "") const {
        auto it = attributes.find(key);
        return it != attributes.end() ? it->second : defaultVal;
    }

    // 序列化为 XML 字符串 / Serialize to XML string
    std::string toString(int indent = 0) const {
        std::string pad(indent * 2, ' ');
        std::ostringstream oss;
        oss << pad << "<" << name;
        for (const auto& [key, val] : attributes) {
            oss << " " << key << "=\"" << escapeXML(val) << "\"";
        }
        if (children.empty() && text.empty()) {
            oss << "/>\n";
        } else {
            oss << ">";
            if (!text.empty()) {
                oss << escapeXML(text);
            }
            if (!children.empty()) {
                oss << "\n";
                for (const auto& child : children) {
                    oss << child.toString(indent + 1);
                }
                oss << pad;
            }
            oss << "</" << name << ">\n";
        }
        return oss.str();
    }

    // 解析 XML / Parse XML
    static XMLNode parse(const std::string& xml) {
        XMLNode root;
        size_t pos = 0;
        parseNode(xml, pos, root);
        return root;
    }

private:
    static void parseNode(const std::string& xml, size_t& pos, XMLNode& node) {
        skipWhitespace(xml, pos);
        if (pos >= xml.size() || xml[pos] != '<') return;
        pos++; // skip '<'

        // 读取标签名 / Read tag name
        size_t nameStart = pos;
        while (pos < xml.size() && xml[pos] != ' ' && xml[pos] != '>' &&
               xml[pos] != '/' && xml[pos] != '\t' && xml[pos] != '\n') pos++;
        node.name = xml.substr(nameStart, pos - nameStart);

        // 读取属性 / Read attributes
        while (pos < xml.size() && xml[pos] != '>' && xml[pos] != '/') {
            skipWhitespace(xml, pos);
            if (xml[pos] == '>' || xml[pos] == '/') break;
            size_t keyStart = pos;
            while (pos < xml.size() && xml[pos] != '=') pos++;
            std::string key = xml.substr(keyStart, pos - keyStart);
            pos++; // skip '='
            if (pos < xml.size() && xml[pos] == '"') pos++;
            size_t valStart = pos;
            while (pos < xml.size() && xml[pos] != '"') pos++;
            std::string value = xml.substr(valStart, pos - valStart);
            if (pos < xml.size()) pos++; // skip '"'
            node.attributes[key] = value;
        }

        // 检查自闭合 / Check self-closing
        if (pos < xml.size() && xml[pos] == '/') {
            pos += 2; // skip '/>'
            return;
        }
        if (pos < xml.size()) pos++; // skip '>'

        // 读取内容 / Read content
        while (pos < xml.size()) {
            if (xml[pos] == '<') {
                if (xml[pos + 1] == '/') {
                    // 结束标签 / End tag
                    pos = xml.find('>', pos) + 1;
                    return;
                } else {
                    // 子节点 / Child node
                    XMLNode child;
                    parseNode(xml, pos, child);
                    node.children.push_back(std::move(child));
                }
            } else {
                // 文本内容 / Text content
                size_t textStart = pos;
                while (pos < xml.size() && xml[pos] != '<') pos++;
                node.text += xml.substr(textStart, pos - textStart);
            }
        }
    }

    static void skipWhitespace(const std::string& s, size_t& pos) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' ||
               s[pos] == '\n' || s[pos] == '\r')) pos++;
    }

    static std::string escapeXML(const std::string& s) {
        std::string result;
        for (char c : s) {
            if (c == '<') result += "&lt;";
            else if (c == '>') result += "&gt;";
            else if (c == '&') result += "&amp;";
            else if (c == '"') result += "&quot;";
            else result += c;
        }
        return result;
    }
};

// ─── CSV ────────────────────────────────────────────────────────────────

// CSV 解析器/序列化器 / CSV parser/serializer
class CSV {
public:
    // 解析 CSV / Parse CSV
    static std::vector<std::vector<std::string>> parse(const std::string& csv,
                                                        char delimiter = ',') {
        std::vector<std::vector<std::string>> rows;
        std::vector<std::string> currentRow;
        std::string currentField;
        bool inQuotes = false;

        for (size_t i = 0; i < csv.size(); i++) {
            char c = csv[i];
            if (inQuotes) {
                if (c == '"') {
                    if (i + 1 < csv.size() && csv[i + 1] == '"') {
                        currentField += '"';
                        i++; // skip next quote
                    } else {
                        inQuotes = false;
                    }
                } else {
                    currentField += c;
                }
            } else {
                if (c == '"') {
                    inQuotes = true;
                } else if (c == delimiter) {
                    currentRow.push_back(currentField);
                    currentField.clear();
                } else if (c == '\n' || c == '\r') {
                    currentRow.push_back(currentField);
                    currentField.clear();
                    if (!currentRow.empty()) {
                        rows.push_back(currentRow);
                        currentRow.clear();
                    }
                    if (c == '\r' && i + 1 < csv.size() && csv[i + 1] == '\n') i++;
                } else {
                    currentField += c;
                }
            }
        }

        // 处理最后一个字段和行
        if (!currentField.empty() || !currentRow.empty()) {
            currentRow.push_back(currentField);
            rows.push_back(currentRow);
        }

        return rows;
    }

    // 序列化为 CSV / Serialize to CSV
    static std::string serialize(const std::vector<std::vector<std::string>>& rows,
                                  char delimiter = ',') {
        std::ostringstream oss;
        for (size_t i = 0; i < rows.size(); i++) {
            for (size_t j = 0; j < rows[i].size(); j++) {
                if (j > 0) oss << delimiter;
                const std::string& field = rows[i][j];
                bool needsQuotes = field.find(delimiter) != std::string::npos ||
                                   field.find('"') != std::string::npos ||
                                   field.find('\n') != std::string::npos;
                if (needsQuotes) {
                    oss << '"';
                    for (char c : field) {
                        if (c == '"') oss << "\"\"";
                        else oss << c;
                    }
                    oss << '"';
                } else {
                    oss << field;
                }
            }
            oss << "\n";
        }
        return oss.str();
    }
};

// ─── MessagePack ────────────────────────────────────────────────────────

// MessagePack 类型 / MessagePack types
enum class MsgPackType : uint8_t {
    Nil = 0xc0,
    False = 0xc2,
    True = 0xc3,
    Int8 = 0xd0,
    Int16 = 0xd1,
    Int32 = 0xd2,
    Int64 = 0xd3,
    Float32 = 0xca,
    Float64 = 0xcb,
    Str8 = 0xd9,
    Str16 = 0xda,
    Str32 = 0xdb,
    Bin8 = 0xc4,
    Bin16 = 0xc5,
    Bin32 = 0xc6,
    Array16 = 0xdc,
    Map16 = 0xde,
};

// MessagePack 编码器 / MessagePack encoder
class MessagePack {
public:
    // 编码整数 / Encode integer
    static std::vector<uint8_t> encodeInt(int64_t value) {
        std::vector<uint8_t> result;
        if (value >= 0 && value <= 127) {
            result.push_back(static_cast<uint8_t>(value));
        } else if (value >= -32 && value < 0) {
            result.push_back(static_cast<uint8_t>(value));
        } else if (value >= -128 && value <= 127) {
            result.push_back(0xd0);
            result.push_back(static_cast<uint8_t>(value));
        } else if (value >= -32768 && value <= 32767) {
            result.push_back(0xd1);
            result.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
            result.push_back(static_cast<uint8_t>(value & 0xFF));
        } else if (value >= -2147483648LL && value <= 2147483647LL) {
            result.push_back(0xd2);
            for (int i = 3; i >= 0; i--) {
                result.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
            }
        } else {
            result.push_back(0xd3);
            for (int i = 7; i >= 0; i--) {
                result.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
            }
        }
        return result;
    }

    // 编码字符串 / Encode string
    static std::vector<uint8_t> encodeString(const std::string& str) {
        std::vector<uint8_t> result;
        size_t len = str.size();
        if (len <= 31) {
            result.push_back(static_cast<uint8_t>(0xa0 | len));
        } else if (len <= 255) {
            result.push_back(0xd9);
            result.push_back(static_cast<uint8_t>(len));
        } else if (len <= 65535) {
            result.push_back(0xda);
            result.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            result.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            result.push_back(0xdb);
            for (int i = 3; i >= 0; i--) {
                result.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
            }
        }
        result.insert(result.end(), str.begin(), str.end());
        return result;
    }

    // 编码数组 / Encode array
    static std::vector<uint8_t> encodeArray(size_t count) {
        std::vector<uint8_t> result;
        if (count <= 15) {
            result.push_back(static_cast<uint8_t>(0x90 | count));
        } else {
            result.push_back(0xdc);
            result.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
            result.push_back(static_cast<uint8_t>(count & 0xFF));
        }
        return result;
    }

    // 编码 map / Encode map
    static std::vector<uint8_t> encodeMap(size_t count) {
        std::vector<uint8_t> result;
        if (count <= 15) {
            result.push_back(static_cast<uint8_t>(0x80 | count));
        } else {
            result.push_back(0xde);
            result.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
            result.push_back(static_cast<uint8_t>(count & 0xFF));
        }
        return result;
    }

    // 编码 nil / Encode nil
    static std::vector<uint8_t> encodeNil() {
        return {0xc0};
    }

    // 编码 bool / Encode bool
    static std::vector<uint8_t> encodeBool(bool value) {
        return {value ? static_cast<uint8_t>(0xc3) : static_cast<uint8_t>(0xc2)};
    }

    // 编码 double / Encode double
    static std::vector<uint8_t> encodeDouble(double value) {
        std::vector<uint8_t> result(9);
        result[0] = 0xcb;
        uint64_t bits;
        std::memcpy(&bits, &value, 8);
        for (int i = 7; i >= 0; i--) {
            result[8 - i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
        }
        return result;
    }

    // 编码二进制 / Encode binary
    static std::vector<uint8_t> encodeBin(const std::vector<uint8_t>& data) {
        std::vector<uint8_t> result;
        size_t len = data.size();
        if (len <= 255) {
            result.push_back(0xc4);
            result.push_back(static_cast<uint8_t>(len));
        } else if (len <= 65535) {
            result.push_back(0xc5);
            result.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            result.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            result.push_back(0xc6);
            for (int i = 3; i >= 0; i--) {
                result.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
            }
        }
        result.insert(result.end(), data.begin(), data.end());
        return result;
    }
};

} // namespace suki::data
