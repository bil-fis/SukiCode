#pragma once
// assert() and precondition() for SukiCode.

#include <string>

namespace suki::stdlib {

// Assert: fatal in debug, no-op in release
[[noreturn]] void assertionFailed(const std::string& message,
                                   const char* file, int line);

} // namespace suki::stdlib

// SukiCode assert macro
#ifdef NDEBUG
#define SUKI_ASSERT(expr) ((void)0)
#else
#define SUKI_ASSERT(expr) \
    do { if (!(expr)) suki::stdlib::assertionFailed(#expr, __FILE__, __LINE__); } while(0)
#endif
