// Coroutine runtime stub.
// Full implementation requires platform-specific context switching.
#include "Coroutine.h"

namespace suki::runtime {

bool CoroutineHandle::done() const {
    // TODO: check coroutine state
    return address == nullptr;
}

void CoroutineHandle::resume() {
    // TODO: resume coroutine execution
}

void CoroutineHandle::destroy() {
    // TODO: cleanup coroutine frame
}

} // namespace suki::runtime
