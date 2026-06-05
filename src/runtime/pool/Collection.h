#pragma once
// Collection — container for Pools with lifecycle management.
// Handles dependency ordering and automatic cleanup with topological sort.

#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace suki::runtime {

class PoolBase {
public:
    virtual ~PoolBase() = default;
    virtual void releaseAll() = 0;
    virtual size_t capacity() const = 0;
    virtual std::string name() const = 0;
};

class Collection {
public:
    Collection();
    ~Collection();

    // 添加池到集合 / Add a pool to the collection
    template<typename P>
    P* add(std::unique_ptr<P> pool) {
        P* raw = pool.get();
        pools_.push_back(std::move(pool));
        return raw;
    }

    // 添加依赖关系 / Add dependency: pool depends on dependency
    void addDependency(PoolBase* pool, PoolBase* dependency);

    // 移除池 / Remove a pool from the collection
    void remove(PoolBase* pool);

    // 关闭：按依赖拓扑序释放所有池 / Shutdown: release all pools in dependency order
    void shutdown();

    // 检查循环依赖 / Check for circular dependencies
    bool hasCircularDependency() const;

    size_t poolCount() const { return pools_.size(); }

private:
    // 拓扑排序 / Topological sort
    std::vector<PoolBase*> topologicalSort() const;

    // DFS 辅助 / DFS helper
    bool dfsVisit(PoolBase* node,
                  std::unordered_set<PoolBase*>& visited,
                  std::unordered_set<PoolBase*>& visiting,
                  std::vector<PoolBase*>& result) const;

    std::vector<std::unique_ptr<PoolBase>> pools_;
    // 依赖图：pool -> [dependencies]
    std::unordered_map<PoolBase*, std::vector<PoolBase*>> dependencies_;
    bool shutdown_ = false;
};

} // namespace suki::runtime
