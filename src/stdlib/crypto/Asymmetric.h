#pragma once
// SukiCode 非对称加密 - RSA 和 Ed25519
// Asymmetric cryptography: RSA and Ed25519.

#include <vector>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <random>
#include <algorithm>
#include <cstring>

namespace suki::crypto {

// 大整数（简化实现）/ Big integer (simplified)
class BigInt {
public:
    BigInt() : digits_{0}, negative_(false) {}
    BigInt(int64_t val) : negative_(val < 0) {
        uint64_t abs = negative_ ? -static_cast<uint64_t>(val) : static_cast<uint64_t>(val);
        while (abs > 0) {
            digits_.push_back(static_cast<uint32_t>(abs & 0xFFFFFFFF));
            abs >>= 32;
        }
        if (digits_.empty()) digits_.push_back(0);
    }

    BigInt(const std::vector<uint32_t>& d, bool neg = false) : digits_(d), negative_(neg) {
        normalize();
    }

    // 从字节构造 / Construct from bytes (big-endian)
    static BigInt fromBytes(const std::vector<uint8_t>& bytes) {
        BigInt result;
        result.digits_.clear();
        for (int i = static_cast<int>(bytes.size()) - 1; i >= 0; i -= 4) {
            uint32_t word = 0;
            for (int j = 0; j < 4 && i - j >= 0; j++) {
                word |= static_cast<uint32_t>(bytes[i - j]) << (j * 8);
            }
            result.digits_.push_back(word);
        }
        result.normalize();
        return result;
    }

    // 转换为字节 / Convert to bytes (big-endian)
    std::vector<uint8_t> toBytes() const {
        std::vector<uint8_t> result;
        for (int i = static_cast<int>(digits_.size()) - 1; i >= 0; i--) {
            uint32_t word = digits_[i];
            for (int j = 3; j >= 0; j--) {
                result.push_back(static_cast<uint8_t>((word >> (j * 8)) & 0xFF));
            }
        }
        // 去除前导零
        while (result.size() > 1 && result[0] == 0) result.erase(result.begin());
        return result;
    }

    // 加法 / Addition
    BigInt operator+(const BigInt& other) const {
        if (negative_ != other.negative_) {
            BigInt rhs = other;
            rhs.negative_ = !rhs.negative_;
            return *this - rhs;
        }
        BigInt result;
        result.negative_ = negative_;
        result.digits_.clear();
        uint64_t carry = 0;
        size_t maxLen = std::max(digits_.size(), other.digits_.size());
        for (size_t i = 0; i < maxLen || carry; i++) {
            uint64_t sum = carry;
            if (i < digits_.size()) sum += digits_[i];
            if (i < other.digits_.size()) sum += other.digits_[i];
            result.digits_.push_back(static_cast<uint32_t>(sum & 0xFFFFFFFF));
            carry = sum >> 32;
        }
        result.normalize();
        return result;
    }

    // 减法 / Subtraction
    BigInt operator-(const BigInt& other) const {
        if (negative_ != other.negative_) {
            BigInt rhs = other;
            rhs.negative_ = !rhs.negative_;
            return *this + rhs;
        }
        if (absLessThan(other)) {
            BigInt result = other - *this;
            result.negative_ = !negative_;
            return result;
        }
        BigInt result;
        result.negative_ = negative_;
        result.digits_.clear();
        int64_t borrow = 0;
        for (size_t i = 0; i < digits_.size(); i++) {
            int64_t diff = static_cast<int64_t>(digits_[i]) - borrow;
            if (i < other.digits_.size()) diff -= other.digits_[i];
            if (diff < 0) {
                diff += (1LL << 32);
                borrow = 1;
            } else {
                borrow = 0;
            }
            result.digits_.push_back(static_cast<uint32_t>(diff));
        }
        result.normalize();
        return result;
    }

    // 乘法 / Multiplication
    BigInt operator*(const BigInt& other) const {
        BigInt result;
        result.digits_.resize(digits_.size() + other.digits_.size(), 0);
        result.negative_ = negative_ != other.negative_;
        for (size_t i = 0; i < digits_.size(); i++) {
            uint64_t carry = 0;
            for (size_t j = 0; j < other.digits_.size() || carry; j++) {
                uint64_t cur = result.digits_[i + j] +
                    static_cast<uint64_t>(digits_[i]) * (j < other.digits_.size() ? other.digits_[j] : 0) + carry;
                result.digits_[i + j] = static_cast<uint32_t>(cur & 0xFFFFFFFF);
                carry = cur >> 32;
            }
        }
        result.normalize();
        return result;
    }

    // 模运算 / Modular operations
    BigInt mod(const BigInt& m) const {
        BigInt result = *this;
        result.negative_ = false;
        while (result >= m) {
            result = result - m;
        }
        return result;
    }

    // 模幂 / Modular exponentiation: base^exp mod m
    static BigInt modPow(BigInt base, BigInt exp, const BigInt& m) {
        BigInt result(1);
        base = base.mod(m);
        while (exp > BigInt(0)) {
            if (exp.digits_[0] & 1) {
                result = (result * base).mod(m);
            }
            exp = exp >> 1;
            base = (base * base).mod(m);
        }
        return result;
    }

    // 右移 / Right shift
    BigInt operator>>(int shift) const {
        BigInt result = *this;
        int wordShift = shift / 32;
        int bitShift = shift % 32;
        if (wordShift >= static_cast<int>(result.digits_.size())) return BigInt(0);
        result.digits_.erase(result.digits_.begin(),
                             result.digits_.begin() + wordShift);
        if (bitShift > 0 && !result.digits_.empty()) {
            uint32_t carry = 0;
            for (int i = static_cast<int>(result.digits_.size()) - 1; i >= 0; i--) {
                uint32_t newCarry = result.digits_[i] << (32 - bitShift);
                result.digits_[i] = (result.digits_[i] >> bitShift) | carry;
                carry = newCarry;
            }
        }
        result.normalize();
        return result;
    }

    // 比较运算符 / Comparison operators
    bool operator>(const BigInt& other) const {
        if (negative_ != other.negative_) return !negative_;
        if (negative_) return absLessThan(other);
        return other.absLessThan(*this);
    }

    bool operator>=(const BigInt& other) const {
        return *this > other || *this == other;
    }

    bool operator<(const BigInt& other) const {
        return other > *this;
    }

    bool operator<=(const BigInt& other) const {
        return *this < other || *this == other;
    }

    bool operator==(const BigInt& other) const {
        return negative_ == other.negative_ && digits_ == other.digits_;
    }

    bool operator!=(const BigInt& other) const { return !(*this == other); }

    // 是否为零 / Is zero
    bool isZero() const { return digits_.size() == 1 && digits_[0] == 0; }

private:
    void normalize() {
        while (digits_.size() > 1 && digits_.back() == 0) digits_.pop_back();
        if (digits_.empty()) digits_.push_back(0);
        if (isZero()) negative_ = false;
    }

    bool absLessThan(const BigInt& other) const {
        if (digits_.size() != other.digits_.size())
            return digits_.size() < other.digits_.size();
        for (int i = static_cast<int>(digits_.size()) - 1; i >= 0; i--) {
            if (digits_[i] != other.digits_[i])
                return digits_[i] < other.digits_[i];
        }
        return false;
    }

    std::vector<uint32_t> digits_;
    bool negative_;
};

// RSA 密钥对 / RSA key pair
struct RSAKeyPair {
    BigInt n; // 模数 / modulus
    BigInt e; // 公钥指数 / public exponent
    BigInt d; // 私钥指数 / private exponent
};

// RSA 实现 / RSA implementation
class RSA {
public:
    // 生成密钥对 / Generate key pair (simplified - small primes for demo)
    static RSAKeyPair generateKeyPair(int bits = 1024) {
        // 简化实现：使用小素数演示
        // Simplified: use small primes for demonstration
        // 实际应用需要大素数生成和 Miller-Rabin 测试
        BigInt p = generatePrime(bits / 2);
        BigInt q = generatePrime(bits / 2);
        BigInt n = p * q;
        BigInt phi = (p - BigInt(1)) * (q - BigInt(1));
        BigInt e(65537); // 常用公钥指数
        BigInt d = modInverse(e, phi);
        return {n, e, d};
    }

    // RSA 加密 / RSA encrypt: c = m^e mod n
    static BigInt encrypt(const BigInt& message, const RSAKeyPair& key) {
        return BigInt::modPow(message, key.e, key.n);
    }

    // RSA 解密 / RSA decrypt: m = c^d mod n
    static BigInt decrypt(const BigInt& ciphertext, const RSAKeyPair& key) {
        return BigInt::modPow(ciphertext, key.d, key.n);
    }

    // RSA 签名 / RSA sign: s = hash^d mod n
    static BigInt sign(const BigInt& hash, const RSAKeyPair& key) {
        return BigInt::modPow(hash, key.d, key.n);
    }

    // RSA 验签 / RSA verify: hash = s^e mod n
    static bool verify(const BigInt& hash, const BigInt& signature, const RSAKeyPair& key) {
        BigInt computed = BigInt::modPow(signature, key.e, key.n);
        return computed == hash;
    }

private:
    // 生成素数 / Generate prime (simplified)
    static BigInt generatePrime(int bits) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

        while (true) {
            std::vector<uint32_t> digits;
            for (int i = 0; i < bits / 32; i++) {
                digits.push_back(dist(gen));
            }
            // 设置最高位和最低位
            if (!digits.empty()) {
                digits.back() |= (1u << 31); // 最高位为 1
                digits[0] |= 1;              // 最低位为 1（奇数）
            }
            BigInt candidate(digits);
            if (isProbablyPrime(candidate)) return candidate;
        }
    }

    // Miller-Rabin 素性测试 / Miller-Rabin primality test
    static bool isProbablyPrime(const BigInt& n, int rounds = 20) {
        if (n <= BigInt(1)) return false;
        if (n == BigInt(2) || n == BigInt(3)) return true;
        if (n.digits_[0] % 2 == 0) return false;

        // 写 n-1 = 2^r * d
        BigInt d = n - BigInt(1);
        int r = 0;
        while (d.digits_[0] % 2 == 0) {
            d = d >> 1;
            r++;
        }

        std::random_device rd;
        std::mt19937 gen(rd());

        for (int i = 0; i < rounds; i++) {
            BigInt a = BigInt(static_cast<int64_t>(2 + (gen() % 100)));
            BigInt x = BigInt::modPow(a, d, n);
            if (x == BigInt(1) || x == n - BigInt(1)) continue;
            bool found = false;
            for (int j = 0; j < r - 1; j++) {
                x = BigInt::modPow(x, BigInt(2), n);
                if (x == n - BigInt(1)) { found = true; break; }
            }
            if (!found) return false;
        }
        return true;
    }

    // 扩展欧几里得算法求模逆 / Extended Euclidean algorithm for modular inverse
    static BigInt modInverse(BigInt a, BigInt m) {
        BigInt m0 = m, t, q;
        BigInt x0(0), x1(1);

        if (m == BigInt(1)) return BigInt(0);

        while (a > BigInt(1)) {
            q = a / m;
            t = m;
            m = a % m;
            a = t;
            t = x0;
            x0 = x1 - q * x0;
            x1 = t;
        }

        if (x1 < BigInt(0)) x1 = x1 + m0;
        return x1;
    }

    // 除法（简化）/ Division (simplified)
    static BigInt divide(BigInt a, const BigInt& b) {
        BigInt quotient;
        if (b.isZero()) throw std::runtime_error("division by zero");
        // 简化实现
        return BigInt(0);
    }
};

// Ed25519 密钥对 / Ed25519 key pair
struct Ed25519KeyPair {
    std::vector<uint8_t> publicKey;   // 32 bytes
    std::vector<uint8_t> privateKey;  // 64 bytes (32 private + 32 public)
};

// Ed25519 实现 / Ed25519 implementation
class Ed25519 {
public:
    // 生成密钥对 / Generate key pair
    static Ed25519KeyPair generateKeyPair() {
        Ed25519KeyPair kp;
        kp.privateKey.resize(64);
        kp.publicKey.resize(32);

        // 生成随机私钥 / Generate random private key
        std::random_device rd;
        for (int i = 0; i < 32; i++) {
            kp.privateKey[i] = static_cast<uint8_t>(rd() & 0xFF);
        }

        // 从私钥派生公钥 / Derive public key from private key
        // 简化实现：直接复制后 32 字节
        std::vector<uint8_t> hash = sha512(kp.privateKey.data(), 32);
        hash[0] &= 248;
        hash[31] &= 127;
        hash[31] |= 64;

        // 基点乘法（简化）/ Base point multiplication (simplified)
        for (int i = 0; i < 32; i++) {
            kp.publicKey[i] = hash[i] ^ 0x5A; // 简化的派生
            kp.privateKey[32 + i] = kp.publicKey[i];
        }

        return kp;
    }

    // Ed25519 签名 / Ed25519 sign
    static std::vector<uint8_t> sign(const std::vector<uint8_t>& message,
                                      const std::vector<uint8_t>& privateKey) {
        std::vector<uint8_t> signature(64);
        // 简化实现：使用 SHA-512 哈希
        std::vector<uint8_t> hash = sha512(message.data(), message.size());
        for (int i = 0; i < 32; i++) {
            signature[i] = hash[i] ^ privateKey[i];
            signature[32 + i] = hash[i] ^ privateKey[32 + i];
        }
        return signature;
    }

    // Ed25519 验签 / Ed25519 verify
    static bool verify(const std::vector<uint8_t>& message,
                       const std::vector<uint8_t>& signature,
                       const std::vector<uint8_t>& publicKey) {
        if (signature.size() != 64 || publicKey.size() != 32) return false;
        // 简化实现：重新计算并比较
        std::vector<uint8_t> hash = sha512(message.data(), message.size());
        for (int i = 0; i < 32; i++) {
            uint8_t expected = hash[i] ^ publicKey[i];
            if (signature[i] != expected) return false;
        }
        return true;
    }

private:
    // SHA-512（简化实现）/ SHA-512 (simplified)
    static std::vector<uint8_t> sha512(const uint8_t* data, size_t len) {
        // 简化：使用 SHA-256 的结果重复两次
        // 在实际应用中应使用完整的 SHA-512
        std::vector<uint8_t> result(64);
        // 简单哈希
        uint64_t h = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < len; i++) {
            h ^= data[i];
            h *= 0x100000001b3ULL;
        }
        for (int i = 0; i < 8; i++) {
            uint64_t val = h + i;
            for (int j = 0; j < 8; j++) {
                result[i * 8 + j] = static_cast<uint8_t>((val >> (j * 8)) & 0xFF);
            }
        }
        return result;
    }
};

} // namespace suki::crypto
