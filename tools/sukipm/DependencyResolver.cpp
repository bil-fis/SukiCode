// SukiPM dependency resolver implementation.

#include "DependencyResolver.h"
#include <queue>
#include <algorithm>

namespace suki::pm {

Resolution DependencyResolver::resolve(const Manifest& manifest) {
    Resolution result;

    // Simple resolution: collect all dependencies
    for (const auto& [name, req] : manifest.dependencies) {
        Resolution::Package pkg;
        pkg.name = name;

        switch (req.kind) {
            case VersionReq::Kind::Exact:
                pkg.version = req.version;
                break;
            case VersionReq::Kind::Range:
                pkg.version = req.version; // TODO: resolve range
                break;
            case VersionReq::Kind::From:
                pkg.version = req.version; // TODO: find minimum compatible version
                break;
            case VersionReq::Kind::Branch:
            case VersionReq::Kind::Revision:
                pkg.source = req.url;
                pkg.version = req.version;
                break;
            case VersionReq::Kind::Path:
                pkg.source = req.localPath;
                pkg.version = "local";
                break;
        }

        result.packages.push_back(pkg);
    }

    // Check for circular dependencies
    std::unordered_map<std::string, std::vector<std::string>> graph;
    for (const auto& pkg : result.packages) {
        graph[pkg.name] = {}; // TODO: resolve transitive dependencies
    }

    if (hasCircularDependency(manifest.name, graph)) {
        result.success = false;
        result.errorMessage = "circular dependency detected";
    }

    return result;
}

void DependencyResolver::addSource(const std::string& name, const std::string& url) {
    sources_[name] = url;
}

bool DependencyResolver::hasCircularDependency(
    const std::string& root,
    const std::unordered_map<std::string, std::vector<std::string>>& graph) {

    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> visiting;

    std::function<bool(const std::string&)> dfs = [&](const std::string& node) -> bool {
        if (visiting.count(node) > 0) return true; // cycle found
        if (visited.count(node) > 0) return false;

        visiting.insert(node);
        auto it = graph.find(node);
        if (it != graph.end()) {
            for (const auto& dep : it->second) {
                if (dfs(dep)) return true;
            }
        }
        visiting.erase(node);
        visited.insert(node);
        return false;
    };

    return dfs(root);
}

} // namespace suki::pm
