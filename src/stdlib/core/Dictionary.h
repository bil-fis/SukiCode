#pragma once
// SukiCode Dictionary<K,V> 值类型 COW 哈希表
// Value-type dictionary with Copy-On-Write optimization.

#include <unordered_map>
#include <memory>
#include <vector>
#include <functional>
#include <initializer_list>

namespace suki::stdlib {

template<typename K, typename V>
class Dictionary {
public:
    // 构造函数 / Constructors
    Dictionary() : data_(std::make_shared<std::unordered_map<K, V>>()) {}
    Dictionary(std::initializer_list<std::pair<const K, V>> init)
        : data_(std::make_shared<std::unordered_map<K, V>>(init)) {}

    // 拷贝/移动 / Copy/Move
    Dictionary(const Dictionary& other) : data_(other.data_) {}
    Dictionary(Dictionary&& other) noexcept : data_(std::move(other.data_)) {}
    Dictionary& operator=(const Dictionary& other) { data_ = other.data_; return *this; }
    Dictionary& operator=(Dictionary&& other) noexcept { data_ = std::move(other.data_); return *this; }

    // 大小 / Size
    size_t count() const { return data_->size(); }
    bool isEmpty() const { return data_->empty(); }

    // 访问 / Access
    V& operator[](const K& key) { ensureUnique(); return (*data_)[key]; }
    const V& operator[](const K& key) const { return (*data_).at(key); }

    // 查找 / Search
    bool contains(const K& key) const { return data_->count(key) > 0; }

    V value(forKey: const K& key, orDefault: const V& defaultValue = V()) const {
        auto it = data_->find(key);
        return it != data_->end() ? it->second : defaultValue;
    }

    // 修改 / Modification
    void set(const K& key, const V& value) { ensureUnique(); (*data_)[key] = value; }
    void remove(const K& key) { ensureUnique(); data_->erase(key); }
    void removeAll() { ensureUnique(); data_->clear(); }

    // 键值对 / Keys and Values
    std::vector<K> keys() const {
        std::vector<K> result;
        for (const auto& pair : *data_) result.push_back(pair.first);
        return result;
    }

    std::vector<V> values() const {
        std::vector<V> result;
        for (const auto& pair : *data_) result.push_back(pair.second);
        return result;
    }

    // 迭代 / Iteration
    auto begin() const { return data_->begin(); }
    auto end() const { return data_->end(); }

    // 比较 / Comparison
    bool operator==(const Dictionary& other) const { return *data_ == *other.data_; }
    bool operator!=(const Dictionary& other) const { return *data_ != *other.data_; }

private:
    void ensureUnique() {
        if (!data_ || data_.use_count() > 1) {
            data_ = std::make_shared<std::unordered_map<K, V>>(*data_);
        }
    }

    std::shared_ptr<std::unordered_map<K, V>> data_;
};

} // namespace suki::stdlib
