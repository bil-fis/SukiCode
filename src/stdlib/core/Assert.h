#pragma once
// assert(), precondition(), and fatalError() for SukiCode.

#include <string>

namespace suki::stdlib {

// Assert: fatal in debug, no-op in release
[[noreturn]] void assertionFailed(const std::string& message,
                                   const char* file, int line);

// Precondition: always checked (even in release), fatal on failure
[[noreturn]] void preconditionFailed(const std::string& message,
                                      const char* file, int line);

// Fatal error: unconditional abort with message
[[noreturn]] void fatalError(const std::string& message,
                              const char* file, int line);

} // namespace suki::stdlib

// SukiCode assert macro (no-op in release)
#ifdef NDEBUG
#define SUKI_ASSERT(expr) ((void)0)
#else
#define SUKI_ASSERT(expr) \
    do { if (!(expr)) suki::stdlib::assertionFailed(#expr, __FILE__, __LINE__); } while(0)
#endif

// Precondition macro (always checked)
#define SUKI_PRECONDITION(expr) \
    do { if (!(expr)) suki::stdlib::preconditionFailed(#expr, __FILE__, __LINE__); } while(0)

// Fatal error macro
#define SUKI_FATAL_ERROR(msg) \
    suki::stdlib::fatalError(msg, __FILE__, __LINE__)
