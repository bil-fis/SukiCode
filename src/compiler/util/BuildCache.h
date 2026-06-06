#pragma once
// SukiCode 增量编译缓存
// Build cache for incremental compilation.

#include <string>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <ctime>

namespace suki {

namespace fs = std::filesystem;

// 文件哈希 / File hash
struct FileHash {
    std::string path;
    uint64_t size;
    std::time_t lastModified;
    std::string contentHash; // SHA256 or similar

    bool operator==(const FileHash& other) const {
        return path == other.path && size == other.size &&
               lastModified == other.lastModified;
    }
};

// 构建缓存 / Build cache
class BuildCache {
public:
    BuildCache(const std::string& cacheDir = ".build/cache")
        : cacheDir_(cacheDir) {
        fs::create_directories(cacheDir);
    }

    // 检查文件是否需要重新编译 / Check if file needs recompilation
    bool needsRebuild(const std::string& filePath) {
        if (!fs::exists(filePath)) return false;

        auto currentHash = computeHash(filePath);
        auto cachedHash = loadHash(filePath);

        if (!cachedHash) return true; // no cache entry
        return !(*cachedHash == currentHash);
    }

    // 标记文件已编译 / Mark file as compiled
    void markCompiled(const std::string& filePath) {
        auto hash = computeHash(filePath);
        saveHash(filePath, hash);
    }

    // 获取缓存的输出路径 / Get cached output path
    std::string getCachedOutput(const std::string& filePath) {
        auto hash = computeHash(filePath);
        return cacheDir_ + "/" + hash.contentHash + ".o";
    }

    // 清除缓存 / Clear cache
    void clear() {
        fs::remove_all(cacheDir_);
        fs::create_directories(cacheDir_);
    }

    // 缓存统计 / Cache statistics
    size_t entryCount() const {
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(cacheDir_)) {
            if (entry.path().extension() == ".hash") count++;
        }
        return count;
    }

private:
    FileHash computeHash(const std::string& filePath) {
        FileHash hash;
        hash.path = filePath;
        auto status = fs::status(filePath);
        hash.size = fs::file_size(filePath);
        hash.lastModified = fs::last_write_time(filePath).time_since_epoch().count();

        // Simple content hash: read file and hash contents
        std::ifstream file(filePath, std::ios::binary);
        if (file) {
            std::string content((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
            hash.contentHash = std::to_string(std::hash<std::string>{}(content));
        }
        return hash;
    }

    std::optional<FileHash> loadHash(const std::string& filePath) {
        std::string hashFile = getHashFilePath(filePath);
        if (!fs::exists(hashFile)) return std::nullopt;

        std::ifstream file(hashFile);
        if (!file) return std::nullopt;

        FileHash hash;
        file >> hash.path >> hash.size >> hash.lastModified >> hash.contentHash;
        return hash;
    }

    void saveHash(const std::string& filePath, const FileHash& hash) {
        std::string hashFile = getHashFilePath(filePath);
        std::ofstream file(hashFile);
        file << hash.path << " " << hash.size << " " << hash.lastModified << " " << hash.contentHash;
    }

    std::string getHashFilePath(const std::string& filePath) {
        // Use a sanitized version of the path as filename
        std::string sanitized = filePath;
        for (char& c : sanitized) {
            if (c == '/' || c == '\\' || c == ':') c = '_';
        }
        return cacheDir_ + "/" + sanitized + ".hash";
    }

    std::string cacheDir_;
};

} // namespace suki
