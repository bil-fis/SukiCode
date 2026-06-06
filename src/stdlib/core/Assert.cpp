// assert(), precondition(), fatalError() implementation.
#include "Assert.h"
#include <iostream>
#include <cstdlib>

namespace suki::stdlib {

[[noreturn]] void assertionFailed(const std::string& message,
                                   const char* file, int line) {
    std::cerr << "Assertion failed: " << message
              << " at " << file << ":" << line << "\n";
    std::abort();
}

[[noreturn]] void preconditionFailed(const std::string& message,
                                      const char* file, int line) {
    std::cerr << "Precondition failed: " << message
              << " at " << file << ":" << line << "\n";
    std::abort();
}

[[noreturn]] void fatalError(const std::string& message,
                              const char* file, int line) {
    std::cerr << "Fatal error: " << message
              << " at " << file << ":" << line << "\n";
    std::abort();
}

} // namespace suki::stdlib
