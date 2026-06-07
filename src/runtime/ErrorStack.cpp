// SukiCode 错误栈实现 / Error stack implementation

#include "ErrorStack.h"

// C 兼容接口实现 / C-compatible interface implementations
extern "C" {

void suki_push_error(void* error) {
    suki::runtime::ErrorStack::instance().push(error);
}

void* suki_pop_error() {
    return suki::runtime::ErrorStack::instance().pop();
}

void* suki_peek_error() {
    return suki::runtime::ErrorStack::instance().peek();
}

int suki_has_error() {
    return suki::runtime::ErrorStack::instance().isEmpty() ? 0 : 1;
}

} // extern "C"
