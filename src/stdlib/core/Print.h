#pragma once
// print() function implementation for SukiCode.
// Thread-safe output to stdout.

#include <string>

namespace suki::stdlib {

// Thread-safe print to stdout
void print(const std::string& message);

// Thread-safe println to stdout
void println(const std::string& message);

} // namespace suki::stdlib
