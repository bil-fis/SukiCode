#pragma once
// SukiCode Path 类型 - 跨平台路径操作
// Cross-platform path manipulation.

#include <string>
#include <vector>
#include <filesystem>

namespace suki::stdlib {

namespace fs = std::filesystem;

class Path {
public:
    Path() = default;
    Path(const std::string& path) : path_(path) {}
    Path(const char* path) : path_(path) {}
    Path(const fs::path& p) : path_(p.string()) {}

    // 路径组件 / Path components
    std::string string() const { return path_; }
    const char* cStr() const { return path_.c_str(); }

    // 路径操作 / Path operations
    Path appending(const std::string& component) const {
        return Path((fs::path(path_) / component).string());
    }

    Path deletingLastComponent() const {
        return Path(fs::path(path_).parent_path().string());
    }

    Path appendingExtension(const std::string& ext) const {
        return Path(fs::path(path_).replace_extension(ext).string());
    }

    Path deletingExtension() const {
        return Path(fs::path(path_).replace_extension().string());
    }

    // 路径信息 / Path info
    std::string lastComponent() const {
        return fs::path(path_).filename().string();
    }

    std::string extension() const {
        return fs::path(path_).extension().string();
    }

    bool isAbsolute() const {
        return fs::path(path_).is_absolute();
    }

    bool isRelative() const {
        return fs::path(path_).is_relative();
    }

    bool isEmpty() const { return path_.empty(); }

    // 路径规范化 / Normalization
    Path normalized() const {
        return Path(fs::path(path_).lexically_normal().string());
    }

    // 绝对路径 / Absolute path
    Path absolute() const {
        try {
            return Path(fs::absolute(path_).string());
        } catch (...) {
            return *this;
        }
    }

    // 相对路径 / Relative path
    Path relativeTo(const Path& base) const {
        try {
            return Path(fs::relative(path_, base.path_).string());
        } catch (...) {
            return *this;
        }
    }

    // 文件系统查询 / Filesystem queries
    bool exists() const {
        return fs::exists(path_);
    }

    bool isFile() const {
        return fs::is_regular_file(path_);
    }

    bool isDirectory() const {
        return fs::is_directory(path_);
    }

    uintmax_t fileSize() const {
        try {
            return fs::file_size(path_);
        } catch (...) {
            return 0;
        }
    }

    // 静态工厂 / Static factories
    static Path currentDirectory() {
        return Path(fs::current_path().string());
    }

    static Path homeDirectory() {
        return Path(fs::path("~").string()); // TODO: proper home dir
    }

    static Path tempDirectory() {
        return Path(fs::temp_directory_path().string());
    }

    // 分隔符 / Separator
    static char separator() {
#ifdef _WIN32
        return '\\';
#else
        return '/';
#endif
    }

    // 比较 / Comparison
    bool operator==(const Path& other) const { return path_ == other.path_; }
    bool operator!=(const Path& other) const { return path_ != other.path_; }
    bool operator<(const Path& other) const { return path_ < other.path_; }

    Path operator/(const Path& other) const {
        return appending(other.path_);
    }

private:
    std::string path_;
};

} // namespace suki::stdlib
