#pragma once
// Collection — container for Pools with lifecycle management.
// Handles dependency ordering and automatic cleanup.

#include <vector>
#include <memory>
#include <string>

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

    // Add a pool to the collection
    template<typename P>
    P* add(std::unique_ptr<P> pool) {
        P* raw = pool.get();
        pools_.push_back(std::move(pool));
        return raw;
    }

    // Remove a pool from the collection
    void remove(PoolBase* pool);

    // Shutdown: release all pools in dependency order
    void shutdown();

    size_t poolCount() const { return pools_.size(); }

private:
    std::vector<std::unique_ptr<PoolBase>> pools_;
    bool shutdown_ = false;
};

} // namespace suki::runtime
