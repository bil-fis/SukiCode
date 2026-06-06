#pragma once
// SukiCode Process 类型 - 进程管理
// Process execution and management.

#include <string>
#include <vector>
#include <functional>

namespace suki::stdlib {

struct ProcessResult {
    int exitCode;
    std::string output;   // stdout output
    std::string errorOutput; // stderr output
    bool success() const { return exitCode == 0; }
};

class Process {
public:
    // 执行命令并等待 / Execute command and wait
    // 通过 2>&1 同时捕获 stdout 和 stderr
    static ProcessResult exec(const std::string& command) {
        ProcessResult result;
        // 重定向 stderr 到 stdout 以同时捕获两者
        std::string cmd = command + " 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            result.exitCode = -1;
            return result;
        }
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            result.output += buffer;
        }
        int status = pclose(pipe);
#ifdef _WIN32
        result.exitCode = status;
#else
        result.exitCode = WEXITSTATUS(status);
#endif
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
        std::vector<std::string> args;
#ifdef _WIN32
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 0; i < argc; i++) {
                char buffer[1024];
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, buffer, sizeof(buffer), nullptr, nullptr);
                args.push_back(buffer);
            }
            LocalFree(argv);
        }
#else
        // On Unix, read /proc/self/cmdline or use argc/argv passed to main
        // For now, read from /proc/self/cmdline
        std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
        if (cmdline.is_open()) {
            std::string arg;
            while (std::getline(cmdline, arg, '\0')) {
                if (!arg.empty()) args.push_back(arg);
            }
        }
#endif
        return args;
    }
};

} // namespace suki::stdlib
