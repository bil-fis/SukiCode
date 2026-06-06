#pragma once
// SukiCode Data 模块 - 字节序列和编解码
// Byte sequences and encoding/decoding.

#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <iomanip>

namespace suki::data {

// Data 类型 - 字节序列 / Byte sequence
class Data {
public:
    Data() = default;
    Data(const uint8_t* ptr, size_t size) : bytes_(ptr, ptr + size) {}
    Data(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    Data(std::vector<uint8_t>&& bytes) : bytes_(std::move(bytes)) {}
    Data(const std::string& str) : bytes_(str.begin(), str.end()) {}

    // 访问 / Access
    const uint8_t* data() const { return bytes_.data(); }
    uint8_t* data() { return bytes_.data(); }
    size_t size() const { return bytes_.size(); }
    bool isEmpty() const { return bytes_.empty(); }

    // 索引 / Indexing
    uint8_t operator[](size_t index) const { return bytes_[index]; }
    uint8_t& operator[](size_t index) { return bytes_[index]; }

    // 修改 / Modification
    void append(const Data& other) {
        bytes_.insert(bytes_.end(), other.bytes_.begin(), other.bytes_.end());
    }
    void append(const uint8_t* ptr, size_t size) {
        bytes_.insert(bytes_.end(), ptr, ptr + size);
    }
    void clear() { bytes_.clear(); }

    // 转换 / Conversion
    std::string toString() const {
        return std::string(bytes_.begin(), bytes_.end());
    }

    // 子数据 / Subdata
    Data subdata(size_t offset, size_t length) const {
        return Data(bytes_.data() + offset, length);
    }

    // 比较 / Comparison
    bool operator==(const Data& other) const { return bytes_ == other.bytes_; }
    bool operator!=(const Data& other) const { return bytes_ != other.bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

// Base64 编解码 / Base64 encoding/decoding
class Base64 {
public:
    static std::string encode(const Data& data) {
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        const uint8_t* bytes = data.data();
        size_t len = data.size();

        for (size_t i = 0; i < len; i += 3) {
            uint32_t n = static_cast<uint32_t>(bytes[i]) << 16;
            if (i + 1 < len) n |= static_cast<uint32_t>(bytes[i + 1]) << 8;
            if (i + 2 < len) n |= static_cast<uint32_t>(bytes[i + 2]);

            result += table[(n >> 18) & 0x3F];
            result += table[(n >> 12) & 0x3F];
            result += (i + 1 < len) ? table[(n >> 6) & 0x3F] : '=';
            result += (i + 2 < len) ? table[n & 0x3F] : '=';
        }

        return result;
    }

    static Data decode(const std::string& encoded) {
        static const uint8_t table[256] = {
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            0,0,0,0,0,0,0,0,0,0,0,62,0,0,0,63,52,53,54,55,56,57,58,59,60,61,0,0,0,0,0,0,
            0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,0,0,0,0,0,
            0,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51
        };

        std::vector<uint8_t> result;
        uint32_t accum = 0;
        int bits = 0;

        for (char c : encoded) {
            if (c == '=') break;
            if (c == ' ' || c == '\n' || c == '\r') continue;

            accum = (accum << 6) | table[static_cast<uint8_t>(c)];
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                result.push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
            }
        }

        return Data(result);
    }
};

// Hex 编解码 / Hex encoding/decoding
class Hex {
public:
    static std::string encode(const Data& data) {
        std::ostringstream oss;
        for (size_t i = 0; i < data.size(); i++) {
            oss << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(data[i]);
        }
        return oss.str();
    }

    static Data decode(const std::string& hex) {
        std::vector<uint8_t> result;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            uint8_t byte = static_cast<uint8_t>(
                std::stoi(hex.substr(i, 2), nullptr, 16));
            result.push_back(byte);
        }
        return Data(result);
    }

    // 比较 / Comparison
    bool operator==(const Data& other) const { return bytes_ == other.bytes_; }
    bool operator!=(const Data& other) const { return bytes_ != other.bytes_; }
    bool operator<(const Data& other) const { return bytes_ < other.bytes_; }

    // 填充 / Fill
    void fill(uint8_t value) { std::fill(bytes_.begin(), bytes_.end(), value); }

    // 调整大小 / Resize
    void resize(size_t newSize, uint8_t fillValue = 0) { bytes_.resize(newSize, fillValue); }

    // 切片 / Slice (returns a copy of the sub-range)
    Data slice(size_t start, size_t length) const {
        if (start >= bytes_.size()) return Data();
        size_t end = std::min(start + length, bytes_.size());
        return Data(std::vector<uint8_t>(bytes_.begin() + start, bytes_.begin() + end));
    }
};

// GZip 压缩 / GZip compression
class GZip {
public:
    // 压缩数据 / Compress data (simple RLE-based compression)
    static Data compress(const Data& input) {
        const auto& bytes = input.bytes();
        std::vector<uint8_t> result;
        // GZip header
        result.push_back(0x1f); result.push_back(0x8b); // magic
        result.push_back(0x08); // deflate method
        result.push_back(0x00); // flags
        result.push_back(0x00); result.push_back(0x00);
        result.push_back(0x00); result.push_back(0x00); // mtime
        result.push_back(0x00); // extra flags
        result.push_back(0xFF); // OS

        // Simple RLE compression body
        size_t i = 0;
        while (i < bytes.size()) {
            uint8_t b = bytes[i];
            size_t run = 1;
            while (i + run < bytes.size() && bytes[i + run] == b && run < 255) {
                run++;
            }
            if (run >= 3) {
                result.push_back(0x00); // literal marker
                result.push_back(static_cast<uint8_t>(run));
                result.push_back(b);
            } else {
                for (size_t j = 0; j < run; j++) {
                    result.push_back(b);
                }
            }
            i += run;
        }

        // CRC32 and size (simplified - use 0)
        for (int j = 0; j < 8; j++) result.push_back(0x00);

        return Data(result);
    }

    // 解压数据 / Decompress data
    static Data decompress(const Data& input) {
        const auto& bytes = input.bytes();
        if (bytes.size() < 18) return Data();

        // Skip GZip header (10 bytes)
        std::vector<uint8_t> result;
        size_t i = 10;
        while (i < bytes.size() - 8) {
            if (bytes[i] == 0x00 && i + 2 < bytes.size() - 8) {
                // RLE encoded
                uint8_t run = bytes[i + 1];
                uint8_t val = bytes[i + 2];
                for (uint8_t j = 0; j < run; j++) {
                    result.push_back(val);
                }
                i += 3;
            } else {
                result.push_back(bytes[i]);
                i++;
            }
        }
        return Data(result);
    }
};

// Zlib 压缩 / Zlib compression
class Zlib {
public:
    // 压缩数据 / Compress data
    static Data compress(const Data& input) {
        const auto& bytes = input.bytes();
        std::vector<uint8_t> result;
        // Zlib header
        result.push_back(0x78); result.push_back(0x01); // deflate, no dict

        // Simple RLE compression body
        size_t i = 0;
        while (i < bytes.size()) {
            uint8_t b = bytes[i];
            size_t run = 1;
            while (i + run < bytes.size() && bytes[i + run] == b && run < 255) {
                run++;
            }
            if (run >= 3) {
                result.push_back(0x00);
                result.push_back(static_cast<uint8_t>(run));
                result.push_back(b);
            } else {
                for (size_t j = 0; j < run; j++) {
                    result.push_back(b);
                }
            }
            i += run;
        }

        // Adler32 checksum (simplified - use 1)
        result.push_back(0x00); result.push_back(0x00);
        result.push_back(0x00); result.push_back(0x01);

        return Data(result);
    }

    // 解压数据 / Decompress data
    static Data decompress(const Data& input) {
        const auto& bytes = input.bytes();
        if (bytes.size() < 6) return Data();

        // Skip Zlib header (2 bytes)
        std::vector<uint8_t> result;
        size_t i = 2;
        while (i < bytes.size() - 4) {
            if (bytes[i] == 0x00 && i + 2 < bytes.size() - 4) {
                uint8_t run = bytes[i + 1];
                uint8_t val = bytes[i + 2];
                for (uint8_t j = 0; j < run; j++) {
                    result.push_back(val);
                }
                i += 3;
            } else {
                result.push_back(bytes[i]);
                i++;
            }
        }
        return Data(result);
    }
};

} // namespace suki::data
