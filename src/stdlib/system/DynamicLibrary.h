#pragma once
// SukiCode DynamicLibrary - 运行时动态库加载
// Runtime dynamic library loading (.so/.dylib/.dll).

#include <string>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace suki::stdlib {

// 动态库句柄 / Dynamic library handle
class DynamicLibrary {
public:
    // 打开动态库 / Open dynamic library
    static DynamicLibrary open(const std::string& path) {
        DynamicLibrary lib;
#ifdef _WIN32
        lib.handle_ = LoadLibraryA(path.c_str());
        if (!lib.handle_) {
            throw std::runtime_error("Failed to load library: " + path);
        }
#else
        lib.handle_ = dlopen(path.c_str(), RTLD_LAZY);
        if (!lib.handle_) {
            throw std::runtime_error(std::string("Failed to load library: ") + dlerror());
        }
#endif
        return lib;
    }

    // 尝试打开动态库（不抛异常）/ Try to open (no exception)
    static DynamicLibrary tryOpen(const std::string& path) {
        DynamicLibrary lib;
#ifdef _WIN32
        lib.handle_ = LoadLibraryA(path.c_str());
#else
        lib.handle_ = dlopen(path.c_str(), RTLD_LAZY);
#endif
        return lib;
    }

    // 查找符号 / Look up symbol
    void* lookup(const std::string& symbol) {
        if (!handle_) return nullptr;
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), symbol.c_str()));
#else
        return dlsym(handle_, symbol.c_str());
#endif
    }

    // 关闭动态库 / Close dynamic library
    void close() {
        if (handle_) {
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(handle_));
#else
            dlclose(handle_);
#endif
            handle_ = nullptr;
        }
    }

    // 是否已打开 / Whether the library is loaded
    bool isOpen() const { return handle_ != nullptr; }

    // 析构时自动关闭 / Auto-close on destruction
    ~DynamicLibrary() { close(); }

    // 移动语义 / Move semantics
    DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    // 禁止拷贝 / No copy
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    // 获取错误信息 / Get last error message
    static std::string lastError() {
#ifdef _WIN32
        return "Error code: " + std::to_string(GetLastError());
#else
        const char* err = dlerror();
        return err ? std::string(err) : "";
#endif
    }

private:
    DynamicLibrary() = default;
    void* handle_ = nullptr;
};

} // namespace suki::stdlib
