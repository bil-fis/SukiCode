#pragma once
// SukiCode File 类型 - 文件读写
// File I/O operations.

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace fs = std::filesystem;

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
    FileHandle(FileHandle&& other) noexcept : stream_(std::move(other.stream_)), isOpen_(other.isOpen_) {
        other.isOpen_ = false;
    }

    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            close();
            stream_ = std::move(other.stream_);
            isOpen_ = other.isOpen_;
            other.isOpen_ = false;
        }
        return *this;
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
        fh.stream_ = std::make_unique<std::fstream>(path, flags);
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

    // 写入字节 / Write bytes
    bool writeBytes(const std::vector<uint8_t>& data) {
        if (!isOpen_) return false;
        stream_->write(reinterpret_cast<const char*>(data.data()), data.size());
        return stream_->good();
    }

    // 读取字节 / Read bytes
    std::vector<uint8_t> readBytes(size_t count) {
        std::vector<uint8_t> result(count);
        if (!isOpen_) return result;
        stream_->read(reinterpret_cast<char*>(result.data()), count);
        result.resize(static_cast<size_t>(stream_->gcount()));
        return result;
    }

    // 读取所有字节 / Read all bytes
    std::vector<uint8_t> readAllBytes() {
        if (!isOpen_) return {};
        auto pos = stream_->tellg();
        stream_->seekg(0, std::ios::end);
        auto size = stream_->tellg();
        stream_->seekg(pos);
        return readBytes(static_cast<size_t>(size - pos));
    }

    // 定位 / Seek
    bool seek(long long offset, std::ios::seekdir origin = std::ios::beg) {
        if (!isOpen_) return false;
        stream_->seekg(offset, origin);
        stream_->seekp(offset, origin);
        return stream_->good();
    }

    // 当前位置 / Tell
    long long tell() {
        if (!isOpen_) return -1;
        return static_cast<long long>(stream_->tellg());
    }

    // 文件大小 / File size
    long long fileSize() {
        if (!isOpen_) return -1;
        auto pos = stream_->tellg();
        stream_->seekg(0, std::ios::end);
        auto size = stream_->tellg();
        stream_->seekg(pos);
        return static_cast<long long>(size);
    }

    // 关闭 / Close
    void close() {
        if (stream_) {
            stream_->close();
            stream_.reset();
        }
        isOpen_ = false;
    }

    bool isOpen() const { return isOpen_; }

private:
    std::unique_ptr<std::fstream> stream_;
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
        return fs::exists(path);
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
        try {
            for (const auto& entry : fs::directory_iterator(path)) {
                entries.push_back(entry.path().filename().string());
            }
        } catch (...) {
            // Directory not found or permission error
        }
        return entries;
    }
};

} // namespace suki::stdlib
