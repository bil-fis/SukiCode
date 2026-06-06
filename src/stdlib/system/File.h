#pragma once
// SukiCode File 类型 - 文件读写
// File I/O operations.

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdint>

namespace suki::stdlib {

enum class FileMode {
    Read,
    Write,
    Append,
    ReadWrite,
};

class FileHandle {
public:
    FileHandle() = default;
    ~FileHandle() { close(); }

    // 禁止拷贝 / No copy
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    // 允许移动 / Allow move
    FileHandle(FileHandle&& other) noexcept : stream_(other.stream_), isOpen_(other.isOpen_) {
        other.stream_ = nullptr;
        other.isOpen_ = false;
    }

    // 打开文件 / Open file
    static FileHandle open(const std::string& path, FileMode mode = FileMode::Read) {
        FileHandle fh;
        std::ios::openmode flags = std::ios::binary;
        switch (mode) {
            case FileMode::Read: flags |= std::ios::in; break;
            case FileMode::Write: flags |= std::ios::out | std::ios::trunc; break;
            case FileMode::Append: flags |= std::ios::out | std::ios::app; break;
            case FileMode::ReadWrite: flags |= std::ios::in | std::ios::out; break;
        }
        fh.stream_ = new std::fstream(path, flags);
        fh.isOpen_ = fh.stream_->is_open();
        return fh;
    }

    // 读取全部内容 / Read all content
    std::string readAll() {
        if (!isOpen_) return "";
        std::ostringstream ss;
        ss << stream_->rdbuf();
        return ss.str();
    }

    // 读取一行 / Read line
    std::string readLine() {
        if (!isOpen_) return "";
        std::string line;
        std::getline(*stream_, line);
        return line;
    }

    // 读取所有行 / Read all lines
    std::vector<std::string> readLines() {
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(*stream_, line)) {
            lines.push_back(line);
        }
        return lines;
    }

    // 写入字符串 / Write string
    bool write(const std::string& data) {
        if (!isOpen_) return false;
        *stream_ << data;
        return stream_->good();
    }

    // 写入行 / Write line
    bool writeLine(const std::string& line) {
        return write(line + "\n");
    }

    // 关闭 / Close
    void close() {
        if (stream_) {
            stream_->close();
            delete stream_;
            stream_ = nullptr;
        }
        isOpen_ = false;
    }

    bool isOpen() const { return isOpen_; }

private:
    std::fstream* stream_ = nullptr;
    bool isOpen_ = false;
};

// 文件系统操作 / Filesystem operations
class FileManager {
public:
    // 读取文件全部内容 / Read entire file
    static std::string readText(const std::string& path) {
        auto fh = FileHandle::open(path, FileMode::Read);
        return fh.readAll();
    }

    // 写入文件 / Write file
    static bool writeText(const std::string& path, const std::string& content) {
        auto fh = FileHandle::open(path, FileMode::Write);
        return fh.write(content);
    }

    // 追加到文件 / Append to file
    static bool appendText(const std::string& path, const std::string& content) {
        auto fh = FileHandle::open(path, FileMode::Append);
        return fh.write(content);
    }

    // 文件是否存在 / Check if file exists
    static bool exists(const std::string& path) {
        std::ifstream f(path);
        return f.good();
    }

    // 删除文件 / Delete file
    static bool remove(const std::string& path) {
        return std::remove(path.c_str()) == 0;
    }

    // 复制文件 / Copy file
    static bool copy(const std::string& from, const std::string& to) {
        std::ifstream src(from, std::ios::binary);
        std::ofstream dst(to, std::ios::binary);
        if (!src.is_open() || !dst.is_open()) return false;
        dst << src.rdbuf();
        return true;
    }

    // 重命名 / Rename
    static bool rename(const std::string& from, const std::string& to) {
        return std::rename(from.c_str(), to.c_str()) == 0;
    }

    // 创建目录 / Create directory
    static bool createDirectory(const std::string& path) {
#ifdef _WIN32
        return _mkdir(path.c_str()) == 0;
#else
        return mkdir(path.c_str(), 0755) == 0;
#endif
    }

    // 列出目录内容 / List directory
    static std::vector<std::string> contentsOfDirectory(const std::string& path) {
        std::vector<std::string> entries;
        // TODO: 实际实现
        return entries;
    }
};

} // namespace suki::stdlib
