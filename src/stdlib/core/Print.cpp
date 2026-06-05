// print() implementation.
#include "Print.h"
#include <iostream>
#include <mutex>

namespace suki::stdlib {

static std::mutex g_printMutex;

void print(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_printMutex);
    std::cout << message;
    std::cout.flush();
}

void println(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_printMutex);
    std::cout << message << "\n";
    std::cout.flush();
}

} // namespace suki::stdlib
