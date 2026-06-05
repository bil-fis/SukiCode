// assert() implementation.
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

} // namespace suki::stdlib
