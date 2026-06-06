#pragma once
// SukiCode Crypto 模块 - 加密算法
// Encryption algorithms: AES-128 ECB, XOR cipher, ChaCha20.

#include <vector>
#include <cstdint>
#include <string>
#include <cstring>

namespace suki::crypto {

// AES-128 ECB 完整实现 / AES-128 ECB full implementation
class AES {
public:
    // AES-128 ECB 模式加密 / AES-128 ECB encryption
    static std::vector<uint8_t> encrypt(const std::vector<uint8_t>& data,
                                         const std::vector<uint8_t>& key) {
        if (key.size() < 16) return data; // 需要 128 位密钥

        std::vector<uint8_t> result;
        std::vector<uint8_t> roundKeys = keyExpansion(key.data());

        // PKCS7 填充 / PKCS7 padding
        std::vector<uint8_t> padded = data;
        size_t padLen = 16 - (padded.size() % 16);
        for (size_t i = 0; i < padLen; i++) {
            padded.push_back(static_cast<uint8_t>(padLen));
        }

        // 加密每个 128 位块 / Encrypt each 128-bit block
        for (size_t i = 0; i < padded.size(); i += 16) {
            uint8_t block[16];
            memcpy(block, &padded[i], 16);
            aesEncryptBlock(block, roundKeys.data());
            result.insert(result.end(), block, block + 16);
        }
        return result;
    }

    // AES-128 ECB 模式解密 / AES-128 ECB decryption
    static std::vector<uint8_t> decrypt(const std::vector<uint8_t>& data,
                                         const std::vector<uint8_t>& key) {
        if (key.size() < 16 || data.size() % 16 != 0) return data;

        std::vector<uint8_t> result;
        std::vector<uint8_t> roundKeys = keyExpansion(key.data());

        // 解密每个 128 位块 / Decrypt each 128-bit block
        for (size_t i = 0; i < data.size(); i += 16) {
            uint8_t block[16];
            memcpy(block, &data[i], 16);
            aesDecryptBlock(block, roundKeys.data());
            result.insert(result.end(), block, block + 16);
        }

        // 移除 PKCS7 填充 / Remove PKCS7 padding
        if (!result.empty()) {
            uint8_t padLen = result.back();
            if (padLen > 0 && padLen <= 16) {
                bool validPad = true;
                for (size_t i = result.size() - padLen; i < result.size(); i++) {
                    if (result[i] != padLen) { validPad = false; break; }
                }
                if (validPad) {
                    result.resize(result.size() - padLen);
                }
            }
        }
        return result;
    }

private:
    // S-Box / S-Box
    static const uint8_t sbox[256];
    static const uint8_t invSbox[256];
    static const uint8_t rcon[11];

    // GF(2^8) 乘法 / GF(2^8) multiplication
    static uint8_t gmul(uint8_t a, uint8_t b) {
        uint8_t p = 0;
        for (int i = 0; i < 8; i++) {
            if (b & 1) p ^= a;
            bool hi = a & 0x80;
            a <<= 1;
            if (hi) a ^= 0x1b;
            b >>= 1;
        }
        return p;
    }

    // 密钥扩展 / Key expansion
    static std::vector<uint8_t> keyExpansion(const uint8_t* key) {
        std::vector<uint8_t> expanded(176);
        memcpy(expanded.data(), key, 16);

        for (int i = 4; i < 44; i++) {
            uint8_t temp[4];
            memcpy(temp, &expanded[(i-1)*4], 4);

            if (i % 4 == 0) {
                // RotWord + SubWord + Rcon
                uint8_t t = temp[0];
                temp[0] = sbox[temp[1]] ^ rcon[i/4];
                temp[1] = sbox[temp[2]];
                temp[2] = sbox[temp[3]];
                temp[3] = sbox[t];
            }

            for (int j = 0; j < 4; j++) {
                expanded[i*4+j] = expanded[(i-4)*4+j] ^ temp[j];
            }
        }
        return expanded;
    }

    // AddRoundKey
    static void addRoundKey(uint8_t* state, const uint8_t* roundKey) {
        for (int i = 0; i < 16; i++) {
            state[i] ^= roundKey[i];
        }
    }

    // SubBytes
    static void subBytes(uint8_t* state) {
        for (int i = 0; i < 16; i++) {
            state[i] = sbox[state[i]];
        }
    }

    // InvSubBytes
    static void invSubBytes(uint8_t* state) {
        for (int i = 0; i < 16; i++) {
            state[i] = invSbox[state[i]];
        }
    }

    // ShiftRows
    static void shiftRows(uint8_t* state) {
        uint8_t temp;
        // Row 1
        temp = state[1]; state[1] = state[5]; state[5] = state[9]; state[9] = state[13]; state[13] = temp;
        // Row 2
        temp = state[2]; state[2] = state[10]; state[10] = temp;
        temp = state[6]; state[6] = state[14]; state[14] = temp;
        // Row 3
        temp = state[15]; state[15] = state[11]; state[11] = state[7]; state[7] = state[3]; state[3] = temp;
    }

    // InvShiftRows
    static void invShiftRows(uint8_t* state) {
        uint8_t temp;
        // Row 1
        temp = state[13]; state[13] = state[9]; state[9] = state[5]; state[5] = state[1]; state[1] = temp;
        // Row 2
        temp = state[2]; state[2] = state[10]; state[10] = temp;
        temp = state[6]; state[6] = state[14]; state[14] = temp;
        // Row 3
        temp = state[3]; state[3] = state[7]; state[7] = state[11]; state[11] = state[15]; state[15] = temp;
    }

    // MixColumns
    static void mixColumns(uint8_t* state) {
        for (int c = 0; c < 4; c++) {
            int i = c * 4;
            uint8_t a0 = state[i], a1 = state[i+1], a2 = state[i+2], a3 = state[i+3];
            state[i]   = gmul(a0,2) ^ gmul(a1,3) ^ a2 ^ a3;
            state[i+1] = a0 ^ gmul(a1,2) ^ gmul(a2,3) ^ a3;
            state[i+2] = a0 ^ a1 ^ gmul(a2,2) ^ gmul(a3,3);
            state[i+3] = gmul(a0,3) ^ a1 ^ a2 ^ gmul(a3,2);
        }
    }

    // InvMixColumns
    static void invMixColumns(uint8_t* state) {
        for (int c = 0; c < 4; c++) {
            int i = c * 4;
            uint8_t a0 = state[i], a1 = state[i+1], a2 = state[i+2], a3 = state[i+3];
            state[i]   = gmul(a0,0x0e) ^ gmul(a1,0x0b) ^ gmul(a2,0x0d) ^ gmul(a3,0x09);
            state[i+1] = gmul(a0,0x09) ^ gmul(a1,0x0e) ^ gmul(a2,0x0b) ^ gmul(a3,0x0d);
            state[i+2] = gmul(a0,0x0d) ^ gmul(a1,0x09) ^ gmul(a2,0x0e) ^ gmul(a3,0x0b);
            state[i+3] = gmul(a0,0x0b) ^ gmul(a1,0x0d) ^ gmul(a2,0x09) ^ gmul(a3,0x0e);
        }
    }

    // 加密单个块 / Encrypt single block
    static void aesEncryptBlock(uint8_t* state, const uint8_t* roundKeys) {
        addRoundKey(state, roundKeys);
        for (int round = 1; round < 10; round++) {
            subBytes(state);
            shiftRows(state);
            mixColumns(state);
            addRoundKey(state, roundKeys + round * 16);
        }
        subBytes(state);
        shiftRows(state);
        addRoundKey(state, roundKeys + 160);
    }

    // 解密单个块 / Decrypt single block
    static void aesDecryptBlock(uint8_t* state, const uint8_t* roundKeys) {
        addRoundKey(state, roundKeys + 160);
        for (int round = 9; round > 1; round--) {
            invShiftRows(state);
            invSubBytes(state);
            addRoundKey(state, roundKeys + round * 16);
            invMixColumns(state);
        }
        invShiftRows(state);
        invSubBytes(state);
        addRoundKey(state, roundKeys);
    }
};

// S-Box / S-Box
inline const uint8_t AES::sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

// Inverse S-Box / Inverse S-Box
inline const uint8_t AES::invSbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

// Rcon / Rcon
inline const uint8_t AES::rcon[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
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

// ChaCha20 完整实现 / ChaCha20 full implementation
class ChaCha20 {
public:
    static std::vector<uint8_t> encrypt(const std::vector<uint8_t>& data,
                                         const std::vector<uint8_t>& key,
                                         const std::vector<uint8_t>& nonce) {
        if (key.size() < 32 || nonce.size() < 12) return data;

        std::vector<uint8_t> result(data.size());
        uint32_t state[16];

        // 初始化状态 / Initialize state
        // "expand 32-byte k"
        state[0] = 0x61707865; state[1] = 0x3320646e;
        state[2] = 0x79622d32; state[3] = 0x6b206574;

        // 密钥 / Key
        for (int i = 0; i < 8; i++) {
            state[4 + i] = (key[i*4]) | (key[i*4+1] << 8) |
                           (key[i*4+2] << 16) | (key[i*4+3] << 24);
        }

        // 计数器 / Counter
        state[12] = 0;

        // Nonce
        for (int i = 0; i < 3; i++) {
            state[13 + i] = (nonce[i*4]) | (nonce[i*4+1] << 8) |
                            (nonce[i*4+2] << 16) | (nonce[i*4+3] << 24);
        }

        // 处理每个 64 字节块 / Process each 64-byte block
        for (size_t block = 0; block < data.size(); block += 64) {
            uint32_t working[16];
            memcpy(working, state, 64);

            // 20 轮 (10 次双轮) / 20 rounds (10 double-rounds)
            for (int i = 0; i < 10; i++) {
                // 列轮 / Column rounds
                quarterRound(working[0], working[4], working[8],  working[12]);
                quarterRound(working[1], working[5], working[9],  working[13]);
                quarterRound(working[2], working[6], working[10], working[14]);
                quarterRound(working[3], working[7], working[11], working[15]);
                // 对角轮 / Diagonal rounds
                quarterRound(working[0], working[5], working[10], working[15]);
                quarterRound(working[1], working[6], working[11], working[12]);
                quarterRound(working[2], working[7], working[8],  working[13]);
                quarterRound(working[3], working[4], working[9],  working[14]);
            }

            // 加初始状态并 XOR / Add initial state and XOR
            for (int i = 0; i < 16; i++) {
                working[i] += state[i];
            }

            // XOR 数据 / XOR data
            size_t remaining = data.size() - block;
            size_t len = remaining < 64 ? remaining : 64;
            for (size_t i = 0; i < len; i++) {
                uint32_t word = working[i / 4];
                uint8_t byte = static_cast<uint8_t>((word >> ((i % 4) * 8)) & 0xFF);
                result[block + i] = data[block + i] ^ byte;
            }

            // 递增计数器 / Increment counter
            state[12]++;
        }
        return result;
    }

private:
    static uint32_t rotl(uint32_t x, int n) {
        return (x << n) | (x >> (32 - n));
    }

    static void quarterRound(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
        a += b; d ^= a; d = rotl(d, 16);
        c += d; b ^= c; b = rotl(b, 12);
        a += b; d ^= a; d = rotl(d, 8);
        c += d; b ^= c; b = rotl(b, 7);
    }
};

} // namespace suki::crypto
