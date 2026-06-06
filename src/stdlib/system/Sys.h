#pragma once
// SukiCode sys 模块 - 系统调用包装
// System call wrappers for low-level operations.

#include <string>
#include <stdexcept>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#endif

namespace suki::stdlib {

// 系统错误 / System error
struct SystemError {
    int code;           // errno or GetLastError()
    std::string message;

    static SystemError fromErrno() {
        SystemError err;
#ifdef _WIN32
        err.code = static_cast<int>(GetLastError());
        char buf[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err.code, 0, buf, sizeof(buf), nullptr);
        err.message = buf;
#else
        err.code = errno;
        err.message = strerror(errno);
#endif
        return err;
    }
};

// 进程管理 / Process management
namespace sys {

// 创建子进程 / Fork (Unix only)
inline int fork() {
#ifdef _WIN32
    return -1; // Windows 不支持 fork
#else
    return ::fork();
#endif
}

// 执行程序 / Exec (Unix only)
inline int exec(const std::string& path, const std::vector<std::string>& args) {
#ifdef _WIN32
    return -1; // Windows 使用 CreateProcess
#else
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    return execvp(path.c_str(), const_cast<char* const*>(argv.data()));
#endif
}

// 等待子进程 / Wait for child process
inline int waitpid(int pid, int* status, int options) {
#ifdef _WIN32
    return -1;
#else
    return ::waitpid(pid, status, options);
#endif
}

// 获取进程 ID / Get process ID
inline int getpid() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return ::getpid();
#endif
}

// 获取父进程 ID / Get parent process ID
inline int getppid() {
#ifdef _WIN32
    return 0;
#else
    return ::getppid();
#endif
}

// 文件 I/O / File I/O
inline int open(const std::string& path, int flags, int mode = 0) {
#ifdef _WIN32
    return -1; // Windows 使用 CreateFile
#else
    return ::open(path.c_str(), flags, mode);
#endif
}

inline int read(int fd, void* buf, size_t count) {
#ifdef _WIN32
    return -1;
#else
    return ::read(fd, buf, count);
#endif
}

inline int write(int fd, const void* buf, size_t count) {
#ifdef _WIN32
    return -1;
#else
    return ::write(fd, buf, count);
#endif
}

inline int close(int fd) {
#ifdef _WIN32
    return -1;
#else
    return ::close(fd);
#endif
}

// 信号处理 / Signal handling
using SignalHandler = void(*)(int);

inline SignalHandler signal(int signum, SignalHandler handler) {
#ifdef _WIN32
    return nullptr;
#else
    return ::signal(signum, handler);
#endif
}

// 时间 / Time
inline int64_t clock_gettime_monotonic() {
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return static_cast<int64_t>(count.QuadPart * 1000000000LL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
#endif
}

inline int nanosleep_ns(int64_t nanoseconds) {
#ifdef _WIN32
    Sleep(static_cast<DWORD>(nanoseconds / 1000000));
    return 0;
#else
    struct timespec ts;
    ts.tv_sec = nanoseconds / 1000000000LL;
    ts.tv_nsec = nanoseconds % 1000000000LL;
    return ::nanosleep(&ts, nullptr);
#endif
}

// 环境变量 / Environment variables
inline std::string getenv(const std::string& name) {
    const char* val = std::getenv(name.c_str());
    return val ? std::string(val) : "";
}

inline void setenv(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    ::setenv(name.c_str(), value.c_str(), 1);
#endif
}

} // namespace sys
} // namespace suki::stdlib
