#pragma once
// SukiCode Process 类型 - 进程管理
// Process execution and management.

#include <string>
#include <vector>
#include <functional>

namespace suki::stdlib {

struct ProcessResult {
    int exitCode;
    std::string stdout;
    std::string stderr;
    bool success() const { return exitCode == 0; }
};

class Process {
public:
    // 执行命令并等待 / Execute command and wait
    static ProcessResult exec(const std::string& command) {
        ProcessResult result;
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            result.exitCode = -1;
            return result;
        }
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            result.stdout += buffer;
        }
        result.exitCode = pclose(pipe);
        return result;
    }

    // 后台执行 / Execute in background
    static int spawn(const std::string& command) {
        return system(command.c_str());
    }

    // 获取环境变量 / Get environment variable
    static std::string env(const std::string& name) {
        const char* val = getenv(name.c_str());
        return val ? std::string(val) : "";
    }

    // 设置环境变量 / Set environment variable
    static void setEnv(const std::string& name, const std::string& value) {
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }

    // 获取当前进程 ID / Get current process ID
    static int pid() {
#ifdef _WIN32
        return _getpid();
#else
        return getpid();
#endif
    }

    // 退出进程 / Exit process
    [[noreturn]] static void exit(int code) {
        std::exit(code);
    }

    // 获取命令行参数 / Get command line arguments
    static std::vector<std::string> arguments() {
        // TODO: 实际实现
        return {};
    }
};

} // namespace suki::stdlib
