#pragma once
// SukiCode Crypto 模块 - 哈希算法
// Hash algorithms: SHA256, MD5, HMAC.

#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <random>
#include <chrono>

namespace suki::crypto {

// SHA256 完整实现 / SHA256 full implementation
class SHA256 {
public:
    static std::vector<uint8_t> hash(const std::vector<uint8_t>& data) {
        // 初始化哈希值 / Initialize hash values
        uint32_t h[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };

        // 常量 / Constants
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
            0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        // 预处理：填充消息 / Pre-processing: padding
        uint64_t bitLen = data.size() * 8;
        std::vector<uint8_t> padded(data);
        padded.push_back(0x80);
        while ((padded.size() % 64) != 56) {
            padded.push_back(0);
        }
        // 追加原始长度（大端）/ Append original length (big-endian)
        for (int i = 56; i >= 0; i -= 8) {
            padded.push_back(static_cast<uint8_t>((bitLen >> i) & 0xFF));
        }

        // 处理每个 512 位块 / Process each 512-bit block
        for (size_t chunk = 0; chunk < padded.size(); chunk += 64) {
            uint32_t w[64];
            // 将块复制到前 16 个字 / Copy block into first 16 words
            for (int i = 0; i < 16; i++) {
                w[i] = (static_cast<uint32_t>(padded[chunk + i * 4]) << 24) |
                       (static_cast<uint32_t>(padded[chunk + i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(padded[chunk + i * 4 + 2]) << 8) |
                       (static_cast<uint32_t>(padded[chunk + i * 4 + 3]));
            }
            // 扩展前 16 个字 / Extend the first 16 words
            for (int i = 16; i < 64; i++) {
                uint32_t s0 = rightRotate(w[i-15], 7) ^ rightRotate(w[i-15], 18) ^ (w[i-15] >> 3);
                uint32_t s1 = rightRotate(w[i-2], 17) ^ rightRotate(w[i-2], 19) ^ (w[i-2] >> 10);
                w[i] = w[i-16] + s0 + w[i-7] + s1;
            }

            uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

            // 压缩函数 / Compression function
            for (int i = 0; i < 64; i++) {
                uint32_t S1 = rightRotate(e, 6) ^ rightRotate(e, 11) ^ rightRotate(e, 25);
                uint32_t ch = (e & f) ^ (~e & g);
                uint32_t temp1 = hh + S1 + ch + k[i] + w[i];
                uint32_t S0 = rightRotate(a, 2) ^ rightRotate(a, 13) ^ rightRotate(a, 22);
                uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t temp2 = S0 + maj;

                hh = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }

            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
            h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        }

        // 产生最终哈希值（大端）/ Produce final hash value (big-endian)
        std::vector<uint8_t> result(32);
        for (int i = 0; i < 8; i++) {
            result[i * 4]     = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
            result[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
            result[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
            result[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFF);
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
    static uint32_t rightRotate(uint32_t value, unsigned int count) {
        return (value >> count) | (value << (32 - count));
    }

    static std::string toHex(const std::vector<uint8_t>& bytes) {
        std::ostringstream oss;
        for (uint8_t b : bytes) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
        }
        return oss.str();
    }
};

// MD5 完整实现 / MD5 full implementation
class MD5 {
public:
    static std::vector<uint8_t> hash(const std::vector<uint8_t>& data) {
        // 初始化 / Initialize
        uint32_t a0 = 0x67452301, b0 = 0xefcdab89;
        uint32_t c0 = 0x98badcfe, d0 = 0x10325476;

        // 预处理 / Pre-processing
        uint64_t bitLen = data.size() * 8;
        std::vector<uint8_t> padded(data);
        padded.push_back(0x80);
        while ((padded.size() % 64) != 56) {
            padded.push_back(0);
        }
        for (int i = 0; i < 8; i++) {
            padded.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFF));
        }

        // 常量 / Constants
        static const uint32_t T[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
            0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
            0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
            0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d7, 0xf4d50d87, 0x455a14ed,
            0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
            0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
            0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
            0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
            0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
        };

        // Shift amounts
        static const int S[64] = {
            7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
            5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
            4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
            6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
        };

        // 处理每个 512 位块 / Process each 512-bit block
        for (size_t chunk = 0; chunk < padded.size(); chunk += 64) {
            uint32_t M[16];
            for (int i = 0; i < 16; i++) {
                M[i] = (static_cast<uint32_t>(padded[chunk + i * 4])) |
                       (static_cast<uint32_t>(padded[chunk + i * 4 + 1]) << 8) |
                       (static_cast<uint32_t>(padded[chunk + i * 4 + 2]) << 16) |
                       (static_cast<uint32_t>(padded[chunk + i * 4 + 3]) << 24);
            }

            uint32_t A = a0, B = b0, C = c0, D = d0;

            for (int i = 0; i < 64; i++) {
                uint32_t F, g;
                if (i < 16) {
                    F = (B & C) | (~B & D);
                    g = i;
                } else if (i < 32) {
                    F = (D & B) | (~D & C);
                    g = (5 * i + 1) % 16;
                } else if (i < 48) {
                    F = B ^ C ^ D;
                    g = (3 * i + 5) % 16;
                } else {
                    F = C ^ (B | ~D);
                    g = (7 * i) % 16;
                }
                F += A + T[i] + M[g];
                A = D;
                D = C;
                C = B;
                B += leftRotate(F, S[i]);
            }

            a0 += A; b0 += B; c0 += C; d0 += D;
        }

        // 输出（小端）/ Output (little-endian)
        std::vector<uint8_t> result(16);
        auto writeLE = [&](uint32_t val, int offset) {
            result[offset]     = static_cast<uint8_t>(val & 0xFF);
            result[offset + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
            result[offset + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
            result[offset + 3] = static_cast<uint8_t>((val >> 24) & 0xFF);
        };
        writeLE(a0, 0); writeLE(b0, 4); writeLE(c0, 8); writeLE(d0, 12);
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

private:
    static uint32_t leftRotate(uint32_t value, unsigned int count) {
        return (value << count) | (value >> (32 - count));
    }
};

// HMAC-SHA256 完整实现 / HMAC-SHA256 full implementation
class HMAC {
public:
    static std::vector<uint8_t> sha256(const std::vector<uint8_t>& key,
                                        const std::vector<uint8_t>& data) {
        // HMAC-SHA256 = SHA256(key XOR opad || SHA256(key XOR ipad || data))
        std::vector<uint8_t> k = key;
        // 如果密钥太长，先哈希 / If key is too long, hash it first
        if (k.size() > 64) {
            k = SHA256::hash(k);
        }
        // 填充到 64 字节 / Pad to 64 bytes
        k.resize(64, 0);

        // 创建 ipad 和 opad / Create ipad and opad
        std::vector<uint8_t> ipad(64, 0x36);
        std::vector<uint8_t> opad(64, 0x5c);
        for (size_t i = 0; i < 64; i++) {
            ipad[i] ^= k[i];
            opad[i] ^= k[i];
        }

        // 内部哈希: SHA256(ipad || data) / Inner hash: SHA256(ipad || data)
        std::vector<uint8_t> innerData;
        innerData.insert(innerData.end(), ipad.begin(), ipad.end());
        innerData.insert(innerData.end(), data.begin(), data.end());
        std::vector<uint8_t> innerHash = SHA256::hash(innerData);

        // 外部哈希: SHA256(opad || innerHash) / Outer hash: SHA256(opad || innerHash)
        std::vector<uint8_t> outerData;
        outerData.insert(outerData.end(), opad.begin(), opad.end());
        outerData.insert(outerData.end(), innerHash.begin(), innerHash.end());
        return SHA256::hash(outerData);
    }
};

// 安全随机数 / Secure random
class SecureRandom {
public:
    static std::vector<uint8_t> bytes(size_t count) {
        std::vector<uint8_t> result(count);
        // 使用硬件随机数生成器 / Use hardware random number generator
        std::random_device rd;
        for (size_t i = 0; i < count; i++) {
            result[i] = static_cast<uint8_t>(rd() & 0xFF);
        }
        return result;
    }

    static uint32_t uint32() {
        std::random_device rd;
        return rd();
    }
};

} // namespace suki::crypto
