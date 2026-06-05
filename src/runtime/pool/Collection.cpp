// Collection implementation.
#include "Collection.h"

namespace suki::runtime {

Collection::Collection() = default;

Collection::~Collection() {
    if (!shutdown_) shutdown();
}

void Collection::remove(PoolBase* pool) {
    for (auto it = pools_.begin(); it != pools_.end(); ++it) {
        if (it->get() == pool) {
            pools_.erase(it);
            return;
        }
    }
}

void Collection::shutdown() {
    if (shutdown_) return;
    shutdown_ = true;

    // Release in reverse order (simple version; proper implementation
    // would use dependency graph topological sort)
    for (auto it = pools_.rbegin(); it != pools_.rend(); ++it) {
        (*it)->releaseAll();
    }
    pools_.clear();
}

} // namespace suki::runtime
