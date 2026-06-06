#pragma once
// SukiPM 依赖解析器
// Dependency resolver using simplified PubGrub algorithm.

#include "Manifest.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace suki::pm {

// 解析结果 / Resolution result
struct Resolution {
    struct Package {
        std::string name;
        std::string version;
        std::string source; // Git URL or local path
    };
    std::vector<Package> packages;
    bool success = true;
    std::string errorMessage;
};

// 依赖解析器 / Dependency resolver
class DependencyResolver {
public:
    DependencyResolver() = default;

    // 解析依赖 / Resolve dependencies
    Resolution resolve(const Manifest& manifest);

    // 添加包源 / Add package source
    void addSource(const std::string& name, const std::string& url);

    // 检查循环依赖 / Check for circular dependencies
    bool hasCircularDependency(const std::string& root,
                                const std::unordered_map<std::string, std::vector<std::string>>& graph);

private:
    // 包源注册表 / Package source registry
    std::unordered_map<std::string, std::string> sources_;

    // 版本缓存 / Version cache
    std::unordered_map<std::string, std::vector<std::string>> versionCache_;
};

} // namespace suki::pm
