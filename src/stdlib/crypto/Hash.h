#pragma once
// SukiCode Crypto 模块 - 哈希算法
// Hash algorithms: SHA256, MD5, HMAC.

#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <iomanip>

namespace suki::crypto {

// SHA256 实现 / SHA256 implementation
class SHA256 {
public:
    static std::vector<uint8_t> hash(const std::vector<uint8_t>& data) {
        // Fallback: 使用标准库哈希（非密码学安全）
        // TODO: 实现完整的 SHA256
        std::hash<std::string> hasher;
        std::string str(data.begin(), data.end());
        size_t h = hasher(str);

        std::vector<uint8_t> result(32, 0);
        for (int i = 0; i < 8 && i < 32; i++) {
            result[i] = static_cast<uint8_t>((h >> (i * 8)) & 0xFF);
        }
        return result;
    }

    static std::vector<uint8_t> hash(const std::string& data) {
        return hash(std::vector<uint8_t>(data.begin(), data.end()));
    }

    static std::string hashHex(const std::string& data) {
        return toHex(hash(data));
    }

private:
    static std::string toHex(const std::vector<uint8_t>& bytes) {
        std::ostringstream oss;
        for (uint8_t b : bytes) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
        }
        return oss.str();
    }
};

// MD5 实现 / MD5 implementation
class MD5 {
public:
    static std::vector<uint8_t> hash(const std::vector<uint8_t>& data) {
        // Fallback: 使用标准库哈希
        std::hash<std::string> hasher;
        std::string str(data.begin(), data.end());
        size_t h = hasher(str);

        std::vector<uint8_t> result(16, 0);
        for (int i = 0; i < 8 && i < 16; i++) {
            result[i] = static_cast<uint8_t>((h >> (i * 8)) & 0xFF);
        }
        return result;
    }

    static std::string hashHex(const std::string& data) {
        auto bytes = hash(std::vector<uint8_t>(data.begin(), data.end()));
        std::ostringstream oss;
        for (uint8_t b : bytes) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
        }
        return oss.str();
    }
};

// HMAC 实现 / HMAC implementation
class HMAC {
public:
    static std::vector<uint8_t> sha256(const std::vector<uint8_t>& key,
                                        const std::vector<uint8_t>& data) {
        // HMAC-SHA256 = SHA256(key XOR opad || SHA256(key XOR ipad || data))
        // 简化实现：直接使用 SHA256
        std::vector<uint8_t> combined;
        combined.insert(combined.end(), key.begin(), key.end());
        combined.insert(combined.end(), data.begin(), data.end());
        return SHA256::hash(combined);
    }
};

// 安全随机数 / Secure random
class SecureRandom {
public:
    static std::vector<uint8_t> bytes(size_t count) {
        std::vector<uint8_t> result(count);
        // TODO: 使用密码学安全的随机数生成器
        for (size_t i = 0; i < count; i++) {
            result[i] = static_cast<uint8_t>(rand() % 256);
        }
        return result;
    }

    static uint32_t uint32() {
        uint32_t result;
        auto bytes = SecureRandom::bytes(4);
        result = (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
        return result;
    }
};

} // namespace suki::crypto
