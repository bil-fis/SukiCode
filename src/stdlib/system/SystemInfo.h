#pragma once
// SukiCode SystemInfo - 操作系统和硬件信息
// OS and hardware information.

#include <string>
#include <cstddef>

#ifdef _WIN32
#include <windows.h>
#elif __APPLE__
#include <sys/sysctl.h>
#include <unistd.h>
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif

namespace suki::stdlib {

// 操作系统信息 / OS information
class OS {
public:
    // 操作系统名称 / OS name
    static std::string name() {
#ifdef _WIN32
        return "Windows";
#elif __APPLE__
        return "macOS";
#elif __linux__
        return "Linux";
#elif __FreeBSD__
        return "FreeBSD";
#else
        return "Unknown";
#endif
    }

    // 操作系统版本 / OS version
    static std::string version() {
#ifdef _WIN32
        OSVERSIONINFOEXA info = {};
        info.dwOSVersionInfoSize = sizeof(info);
        // 使用 RtlGetVersion 获取真实版本
        return "Windows";
#elif __APPLE__
        char buf[64];
        size_t size = sizeof(buf);
        if (sysctlbyname("kern.osproductversion", buf, &size, nullptr, 0) == 0) {
            return std::string(buf);
        }
        return "Unknown";
#else
        struct utsname uts;
        if (uname(&uts) == 0) {
            return std::string(uts.release);
        }
        return "Unknown";
#endif
    }

    // 架构 / Architecture
    static std::string architecture() {
#if defined(_M_X64) || defined(__x86_64__)
        return "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
        return "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
        return "arm64";
#elif defined(__arm__)
        return "arm";
#elif defined(__riscv) && (__riscv_xlen == 64)
        return "riscv64";
#else
        return "Unknown";
#endif
    }

    // 主机名 / Hostname
    static std::string hostname() {
        char buf[256];
#ifdef _WIN32
        DWORD size = sizeof(buf);
        if (GetComputerNameA(buf, &size)) return std::string(buf);
#else
        if (gethostname(buf, sizeof(buf)) == 0) return std::string(buf);
#endif
        return "Unknown";
    }

    // 当前用户名 / Current username
    static std::string username() {
#ifdef _WIN32
        char buf[256];
        DWORD size = sizeof(buf);
        if (GetUserNameA(buf, &size)) return std::string(buf);
#else
        const char* user = getenv("USER");
        if (user) return std::string(user);
#endif
        return "Unknown";
    }

    // 路径分隔符 / Path separator
    static char pathSeparator() {
#ifdef _WIN32
        return '\\';
#else
        return '/';
#endif
    }

    // 行结束符 / Line ending
    static const char* lineEnding() {
#ifdef _WIN32
        return "\r\n";
#else
        return "\n";
#endif
    }

    // 是否是 Windows / Is Windows
    static constexpr bool isWindows() {
#ifdef _WIN32
        return true;
#else
        return false;
#endif
    }

    // 是否是 macOS / Is macOS
    static constexpr bool isMacOS() {
#ifdef __APPLE__
        return true;
#else
        return false;
#endif
    }

    // 是否是 Linux / Is Linux
    static constexpr bool isLinux() {
#ifdef __linux__
        return true;
#else
        return false;
#endif
    }
};

// 硬件信息 / Hardware information
class Hardware {
public:
    // CPU 核心数 / Number of CPU cores
    static size_t cpuCoreCount() {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return info.dwNumberOfProcessors;
#elif __APPLE__
        int cores = 0;
        size_t size = sizeof(cores);
        if (sysctlbyname("hw.ncpu", &cores, &size, nullptr, 0) == 0) {
            return static_cast<size_t>(cores);
        }
        return 1;
#else
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        return cores > 0 ? static_cast<size_t>(cores) : 1;
#endif
    }

    // 物理内存大小（字节）/ Physical memory size in bytes
    static size_t physicalMemory() {
#ifdef _WIN32
        MEMORYSTATUSEX status = {};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) {
            return status.ullTotalPhys;
        }
        return 0;
#elif __APPLE__
        uint64_t mem = 0;
        size_t size = sizeof(mem);
        if (sysctlbyname("hw.memsize", &mem, &size, nullptr, 0) == 0) {
            return mem;
        }
        return 0;
#else
        long pages = sysconf(_SC_PHYS_PAGES);
        long pageSize = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && pageSize > 0) {
            return static_cast<size_t>(pages) * static_cast<size_t>(pageSize);
        }
        return 0;
#endif
    }

    // 页面大小（字节）/ Page size in bytes
    static size_t pageSize() {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return info.dwPageSize;
#else
        long size = sysconf(_SC_PAGE_SIZE);
        return size > 0 ? static_cast<size_t>(size) : 4096;
#endif
    }
};

} // namespace suki::stdlib
