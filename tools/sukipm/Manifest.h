#pragma once
// SukiPM 清单文件解析器
// Manifest file parser for .sukiproj files.

#include <string>
#include <vector>
#include <unordered_map>

namespace suki::pm {

// 依赖版本要求 / Dependency version requirement
struct VersionReq {
    enum class Kind { Exact, Range, From, Branch, Revision, Path };
    Kind kind;
    std::string version;    // "1.2.3" or "1.0.0"..."2.0.0"
    std::string url;        // Git URL
    std::string branch;     // Branch name
    std::string revision;   // Commit hash
    std::string localPath;  // Local path
    std::string registry;   // Registry name
};

// 目标类型 / Target type
enum class TargetType { Executable, Library, Test };

// 构建目标 / Build target
struct Target {
    std::string name;
    TargetType type;
    std::vector<std::string> dependencies;
    std::string path; // source path
};

// 平台要求 / Platform requirement
enum class Platform { macOS, Linux, Windows, iOS, Android };

// 项目清单 / Project manifest
struct Manifest {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> authors;
    std::string license;

    // 目标 / Targets
    std::string defaultTarget;
    std::vector<Target> targets;

    // 依赖 / Dependencies
    std::unordered_map<std::string, VersionReq> dependencies;

    // 平台 / Platforms
    std::vector<Platform> platforms;

    // 解析清单文件 / Parse manifest file
    static Manifest parse(const std::string& content);

    // 生成清单文件 / Generate manifest file
    std::string dump() const;
};

} // namespace suki::pm
