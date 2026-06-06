#pragma once
// SukiCode Crypto 模块 - 加密算法
// Encryption algorithms: AES, XOR cipher.

#include <vector>
#include <cstdint>
#include <string>

namespace suki::crypto {

// AES 加密（简化实现）/ AES encryption (simplified)
class AES {
public:
    // AES-128 ECB 模式加密 / AES-128 ECB encryption
    static std::vector<uint8_t> encrypt(const std::vector<uint8_t>& data,
                                         const std::vector<uint8_t>& key) {
        // 简化实现：XOR 加密（实际应用需使用完整 AES）
        // Simplified: XOR encryption (real app needs full AES)
        std::vector<uint8_t> result(data.size());
        for (size_t i = 0; i < data.size(); i++) {
            result[i] = data[i] ^ key[i % key.size()];
        }
        return result;
    }

    // AES-128 ECB 模式解密 / AES-128 ECB decryption
    static std::vector<uint8_t> decrypt(const std::vector<uint8_t>& data,
                                         const std::vector<uint8_t>& key) {
        // XOR 解密与加密相同
        return encrypt(data, key);
    }
};

// XOR 密码 / XOR Cipher
class XORCipher {
public:
    static std::vector<uint8_t> encrypt(const std::vector<uint8_t>& data,
                                         uint8_t key) {
        std::vector<uint8_t> result(data.size());
        for (size_t i = 0; i < data.size(); i++) {
            result[i] = data[i] ^ key;
        }
        return result;
    }

    static std::vector<uint8_t> decrypt(const std::vector<uint8_t>& data,
                                         uint8_t key) {
        return encrypt(data, key);
    }
};

// ChaCha20（简化实现）/ ChaCha20 (simplified)
class ChaCha20 {
public:
    static std::vector<uint8_t> encrypt(const std::vector<uint8_t>& data,
                                         const std::vector<uint8_t>& key,
                                         const std::vector<uint8_t>& nonce) {
        // 简化实现：使用密钥流 XOR
        std::vector<uint8_t> result(data.size());
        std::vector<uint8_t> keystream(data.size());

        // 生成简单密钥流
        for (size_t i = 0; i < data.size(); i++) {
            keystream[i] = key[i % key.size()] ^ nonce[i % nonce.size()] ^ (uint8_t)i;
        }

        for (size_t i = 0; i < data.size(); i++) {
            result[i] = data[i] ^ keystream[i];
        }
        return result;
    }
};

} // namespace suki::crypto
