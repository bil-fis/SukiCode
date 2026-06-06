#pragma once
// SukiCode MMapRegion - 内存映射文件/匿名内存
// Memory-mapped file and anonymous memory.

#include <string>
#include <cstdint>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace suki::stdlib {

// 内存映射保护模式 / Memory mapping protection
enum class MMapProt : int {
    None = 0,
    Read = 1,
    Write = 2,
    Exec = 4,
    ReadWrite = 3,
    ReadExec = 5,
};

// 内存映射标志 / Memory mapping flags
enum class MMapFlags : int {
    Shared = 1,
    Private = 2,
    Anonymous = 4,
    Fixed = 8,
};

// 内存映射区域 / Memory-mapped region
class MMapRegion {
public:
    // 映射文件 / Map file
    static MMapRegion mapFile(const std::string& path, MMapProt prot = MMapProt::ReadWrite) {
        MMapRegion region;
#ifdef _WIN32
        DWORD access = GENERIC_READ;
        if (static_cast<int>(prot) & static_cast<int>(MMapProt::Write))
            access |= GENERIC_WRITE;
        DWORD protect = PAGE_READONLY;
        if (static_cast<int>(prot) & static_cast<int>(MMapProt::Write))
            protect = PAGE_READWRITE;

        HANDLE file = CreateFileA(path.c_str(), access, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to open file: " + path);
        }

        LARGE_INTEGER fileSize;
        GetFileSizeEx(file, &fileSize);
        region.size_ = static_cast<size_t>(fileSize.QuadPart);

        HANDLE mapping = CreateFileMappingA(file, nullptr, protect, 0, 0, nullptr);
        CloseHandle(file);
        if (!mapping) {
            throw std::runtime_error("Failed to create file mapping");
        }

        DWORD viewAccess = FILE_MAP_READ;
        if (static_cast<int>(prot) & static_cast<int>(MMapProt::Write))
            viewAccess = FILE_MAP_ALL_ACCESS;

        region.data_ = static_cast<uint8_t*>(MapViewOfFile(mapping, viewAccess, 0, 0, region.size_));
        CloseHandle(mapping);
        if (!region.data_) {
            throw std::runtime_error("Failed to map view of file");
        }
#else
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("Failed to open file: " + path);
        }

        struct stat st;
        fstat(fd, &st);
        region.size_ = static_cast<size_t>(st.st_size);

        int flags = MAP_PRIVATE;
        int posixProt = PROT_READ;
        if (static_cast<int>(prot) & static_cast<int>(MMapProt::Write)) {
            posixProt |= PROT_WRITE;
            flags = MAP_SHARED;
        }

        region.data_ = static_cast<uint8_t*>(
            mmap(nullptr, region.size_, posixProt, flags, fd, 0));
        close(fd);

        if (region.data_ == MAP_FAILED) {
            region.data_ = nullptr;
            throw std::runtime_error("Failed to mmap file");
        }
#endif
        region.ownsData_ = true;
        return region;
    }

    // 匿名映射 / Anonymous mapping
    static MMapRegion mapAnonymous(size_t size, MMapProt prot = MMapProt::ReadWrite) {
        MMapRegion region;
        region.size_ = size;
#ifdef _WIN32
        DWORD protect = PAGE_READWRITE;
        region.data_ = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, protect));
        if (!region.data_) {
            throw std::runtime_error("Failed to allocate virtual memory");
        }
#else
        int posixProt = PROT_READ | PROT_WRITE;
        if (static_cast<int>(prot) & static_cast<int>(MMapProt::Exec))
            posixProt |= PROT_EXEC;

        region.data_ = static_cast<uint8_t*>(
            mmap(nullptr, size, posixProt, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (region.data_ == MAP_FAILED) {
            region.data_ = nullptr;
            throw std::runtime_error("Failed to mmap anonymous memory");
        }
#endif
        region.ownsData_ = true;
        return region;
    }

    // 析构时取消映射 / Unmap on destruction
    ~MMapRegion() { unmap(); }

    // 移动语义 / Move semantics
    MMapRegion(MMapRegion&& other) noexcept
        : data_(other.data_), size_(other.size_), ownsData_(other.ownsData_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.ownsData_ = false;
    }

    MMapRegion& operator=(MMapRegion&& other) noexcept {
        if (this != &other) {
            unmap();
            data_ = other.data_;
            size_ = other.size_;
            ownsData_ = other.ownsData_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.ownsData_ = false;
        }
        return *this;
    }

    // 禁止拷贝 / No copy
    MMapRegion(const MMapRegion&) = delete;
    MMapRegion& operator=(const MMapRegion&) = delete;

    // 数据访问 / Data access
    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    bool isValid() const { return data_ != nullptr && size_ > 0; }

    // 同步到磁盘 / Sync to disk
    void sync() {
#ifdef _WIN32
        if (data_ && size_) FlushViewOfFile(data_, size_);
#else
        if (data_ && size_) msync(data_, size_, MS_SYNC);
#endif
    }

    // 取消映射 / Unmap
    void unmap() {
        if (!data_ || !ownsData_) return;
#ifdef _WIN32
        UnmapViewOfFile(data_);
#else
        munmap(data_, size_);
#endif
        data_ = nullptr;
        size_ = 0;
        ownsData_ = false;
    }

private:
    MMapRegion() = default;
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool ownsData_ = false;
};

} // namespace suki::stdlib
