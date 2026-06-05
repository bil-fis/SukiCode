// Collection implementation with dependency graph topological sort.

#include "Collection.h"
#include <algorithm>
#include <iostream>

namespace suki::runtime {

Collection::Collection() = default;

Collection::~Collection() {
    if (!shutdown_) shutdown();
}

void Collection::addDependency(PoolBase* pool, PoolBase* dependency) {
    dependencies_[pool].push_back(dependency);
}

void Collection::remove(PoolBase* pool) {
    for (auto it = pools_.begin(); it != pools_.end(); ++it) {
        if (it->get() == pool) {
            pools_.erase(it);
            dependencies_.erase(pool);
            return;
        }
    }
}

void Collection::shutdown() {
    if (shutdown_) return;
    shutdown_ = true;

    // 检查循环依赖 / Check for circular dependencies
    if (hasCircularDependency()) {
        std::cerr << "Collection: circular dependency detected, forcing shutdown\n";
        // 强制逆序释放 / Force reverse order release
        for (auto it = pools_.rbegin(); it != pools_.rend(); ++it) {
            (*it)->releaseAll();
        }
        pools_.clear();
        return;
    }

    // 按拓扑序释放 / Release in topological order
    auto sorted = topologicalSort();
    for (auto* pool : sorted) {
        pool->releaseAll();
    }
    pools_.clear();
}

bool Collection::hasCircularDependency() const {
    std::unordered_set<PoolBase*> visited;
    std::unordered_set<PoolBase*> visiting;

    for (const auto& pool : pools_) {
        if (visited.find(pool.get()) == visited.end()) {
            if (dfsVisit(pool.get(), visited, visiting,
                         const_cast<std::vector<PoolBase*>&>(
                             *reinterpret_cast<const std::vector<PoolBase*>*>(&pools_)))) {
                return true;
            }
        }
    }
    return false;
}

std::vector<PoolBase*> Collection::topologicalSort() const {
    std::vector<PoolBase*> result;
    std::unordered_set<PoolBase*> visited;
    std::unordered_set<PoolBase*> visiting;

    for (const auto& pool : pools_) {
        if (visited.find(pool.get()) == visited.end()) {
            dfsVisit(pool.get(), visited, visiting, result);
        }
    }

    // 逆序：先释放依赖者，再释放被依赖者
    // Reverse: release dependents first, then dependencies
    std::reverse(result.begin(), result.end());
    return result;
}

bool Collection::dfsVisit(PoolBase* node,
                           std::unordered_set<PoolBase*>& visited,
                           std::unordered_set<PoolBase*>& visiting,
                           std::vector<PoolBase*>& result) const {
    if (visiting.find(node) != visiting.end()) {
        // 检测到循环 / Cycle detected
        return true;
    }
    if (visited.find(node) != visited.end()) {
        return false;
    }

    visiting.insert(node);

    // 访问依赖 / Visit dependencies
    auto it = dependencies_.find(node);
    if (it != dependencies_.end()) {
        for (auto* dep : it->second) {
            if (dfsVisit(dep, visited, visiting, result)) {
                return true;
            }
        }
    }

    visiting.erase(node);
    visited.insert(node);
    result.push_back(node);
    return false;
}

} // namespace suki::runtime
