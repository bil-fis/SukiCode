#pragma once
// SukiCode CLI 模块 - 命令行参数解析
// Command line argument parsing.

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <algorithm>

namespace suki::cli {

// 参数类型 / Argument types
enum class ArgType { String, Int, Double, Bool, Counter };

// 参数定义 / Argument definition
struct ArgDef {
    std::string longName;      // --verbose
    char shortName = 0;        // -v
    std::string description;
    ArgType type = ArgType::String;
    bool required = false;
    std::string defaultValue;
};

// 解析结果 / Parse result
class ParseResult {
public:
    bool has(const std::string& name) const {
        return values_.count(name) > 0;
    }

    std::string getString(const std::string& name, const std::string& defaultVal = "") const {
        auto it = values_.find(name);
        return it != values_.end() ? it->second : defaultVal;
    }

    int getInt(const std::string& name, int defaultVal = 0) const {
        auto it = values_.find(name);
        if (it != values_.end()) {
            try { return std::stoi(it->second); } catch (...) {}
        }
        return defaultVal;
    }

    double getDouble(const std::string& name, double defaultVal = 0.0) const {
        auto it = values_.find(name);
        if (it != values_.end()) {
            try { return std::stod(it->second); } catch (...) {}
        }
        return defaultVal;
    }

    bool getBool(const std::string& name, bool defaultVal = false) const {
        auto it = values_.find(name);
        if (it != values_.end()) {
            return it->second == "true" || it->second == "1";
        }
        return defaultVal;
    }

    std::vector<std::string> positionalArgs() const { return positional_; }

    void set(const std::string& name, const std::string& value) {
        values_[name] = value;
    }

    void addPositional(const std::string& arg) {
        positional_.push_back(arg);
    }

private:
    std::unordered_map<std::string, std::string> values_;
    std::vector<std::string> positional_;
};

// 参数解析器 / Argument parser
class ArgumentParser {
public:
    ArgumentParser(const std::string& program, const std::string& description = "")
        : program_(program), description_(description) {}

    // 添加参数 / Add argument
    ArgumentParser& addArgument(const ArgDef& def) {
        args_.push_back(def);
        if (def.shortName) shortMap_[def.shortName] = def.longName;
        return *this;
    }

    // 便捷方法 / Convenience methods
    ArgumentParser& addString(const std::string& name, const std::string& desc, bool required = false) {
        return addArgument({name, 0, desc, ArgType::String, required});
    }

    ArgumentParser& addInt(const std::string& name, const std::string& desc, bool required = false) {
        return addArgument({name, 0, desc, ArgType::Int, required});
    }

    ArgumentParser& addBool(const std::string& name, char shortName, const std::string& desc) {
        return addArgument({name, shortName, desc, ArgType::Bool, false});
    }

    ArgumentParser& addCounter(const std::string& name, char shortName, const std::string& desc) {
        return addArgument({name, shortName, desc, ArgType::Counter, false});
    }

    // 解析 / Parse
    ParseResult parse(int argc, char* argv[]) {
        ParseResult result;

        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];

            if (arg == "-h" || arg == "--help") {
                printHelp();
                exit(0);
            }

            if (arg.substr(0, 2) == "--") {
                // Long option
                std::string name = arg.substr(2);
                size_t eqPos = name.find('=');
                if (eqPos != std::string::npos) {
                    result.set(name.substr(0, eqPos), name.substr(eqPos + 1));
                } else {
                    // Check if next arg is value
                    auto def = findArg(name);
                    if (def && def->type != ArgType::Bool && def->type != ArgType::Counter) {
                        if (i + 1 < argc) {
                            result.set(name, argv[++i]);
                        }
                    } else {
                        result.set(name, "true");
                    }
                }
            } else if (arg[0] == '-' && arg.size() > 1) {
                // Short option
                char shortName = arg[1];
                auto it = shortMap_.find(shortName);
                if (it != shortMap_.end()) {
                    auto def = findArg(it->second);
                    if (def && def->type != ArgType::Bool && def->type != ArgType::Counter) {
                        if (i + 1 < argc) {
                            result.set(it->second, argv[++i]);
                        }
                    } else {
                        result.set(it->second, "true");
                    }
                }
            } else {
                result.addPositional(arg);
            }
        }

        // Set defaults
        for (const auto& def : args_) {
            if (!def.defaultValue.empty() && !result.has(def.longName)) {
                result.set(def.longName, def.defaultValue);
            }
        }

        return result;
    }

    // 打印帮助 / Print help
    void printHelp() const {
        std::cout << program_;
        if (!description_.empty()) std::cout << " - " << description_;
        std::cout << "\n\n";

        std::cout << "Usage:\n";
        std::cout << "  " << program_ << " [options]";
        for (const auto& arg : args_) {
            if (arg.required) std::cout << " <" << arg.longName << ">";
        }
        std::cout << "\n\n";

        std::cout << "Options:\n";
        for (const auto& arg : args_) {
            std::cout << "  ";
            if (arg.shortName) std::cout << "-" << arg.shortName << ", ";
            else std::cout << "    ";
            std::cout << "--" << arg.longName;
            if (arg.type != ArgType::Bool && arg.type != ArgType::Counter) {
                std::cout << " <value>";
            }
            std::cout << "  " << arg.description;
            if (!arg.defaultValue.empty()) {
                std::cout << " (default: " << arg.defaultValue << ")";
            }
            std::cout << "\n";
        }
    }

private:
    const ArgDef* findArg(const std::string& name) const {
        for (const auto& arg : args_) {
            if (arg.longName == name) return &arg;
        }
        return nullptr;
    }

    std::string program_;
    std::string description_;
    std::vector<ArgDef> args_;
    std::unordered_map<char, std::string> shortMap_;
};

// 终端颜色 / Terminal colors
namespace color {
    const char* reset = "\033[0m";
    const char* red = "\033[31m";
    const char* green = "\033[32m";
    const char* yellow = "\033[33m";
    const char* blue = "\033[34m";
    const char* magenta = "\033[35m";
    const char* cyan = "\033[36m";
    const char* bold = "\033[1m";
}

} // namespace suki::cli
