#pragma once
// SukiCode Test 模块 - 单元测试框架
// Unit testing framework.

#include <string>
#include <vector>
#include <functional>
#include <iostream>
#include <chrono>
#include <cmath>
#include <sstream>

namespace suki::test {

// 测试结果 / Test result
struct TestResult {
    std::string name;
    bool passed;
    std::string message;
    double durationMs;
};

// 测试套件 / Test suite
class TestSuite {
public:
    static TestSuite& instance() {
        static TestSuite suite;
        return suite;
    }

    // 注册测试 / Register test
    void addTest(const std::string& name, std::function<void()> test) {
        tests_.push_back({name, std::move(test)});
    }

    // 运行所有测试 / Run all tests
    int runAll() {
        int passed = 0;
        int failed = 0;
        std::vector<TestResult> results;

        for (const auto& test : tests_) {
            TestResult result;
            result.name = test.name;

            auto start = std::chrono::high_resolution_clock::now();
            try {
                test.func();
                result.passed = true;
                passed++;
            } catch (const std::exception& e) {
                result.passed = false;
                result.message = e.what();
                failed++;
            } catch (...) {
                result.passed = false;
                result.message = "unknown exception";
                failed++;
            }
            auto end = std::chrono::high_resolution_clock::now();
            result.durationMs = std::chrono::duration<double, std::milli>(end - start).count();

            results.push_back(result);

            // 输出结果 / Print result
            if (result.passed) {
                std::cout << "  PASS: " << result.name
                          << " (" << result.durationMs << "ms)" << std::endl;
            } else {
                std::cerr << "  FAIL: " << result.name
                          << " - " << result.message << std::endl;
            }
        }

        // 总结 / Summary
        std::cout << "\n" << passed << " passed, " << failed << " failed, "
                  << tests_.size() << " total" << std::endl;

        return failed;
    }

private:
    struct TestEntry {
        std::string name;
        std::function<void()> func;
    };
    std::vector<TestEntry> tests_;
};

// 断言异常 / Assertion exception
class AssertionError : public std::exception {
public:
    AssertionError(const std::string& msg) : message_(msg) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

// ─── 断言函数 / Assertion functions ───────────────────────────────────────

inline void expect(bool condition, const std::string& msg = "assertion failed") {
    if (!condition) throw AssertionError(msg);
}

inline void assertEqual(int expected, int actual, const std::string& msg = "") {
    if (expected != actual) {
        std::ostringstream oss;
        oss << "expected " << expected << " but got " << actual;
        if (!msg.empty()) oss << " (" << msg << ")";
        throw AssertionError(oss.str());
    }
}

inline void assertEqual(double expected, double actual, double tolerance = 1e-9, const std::string& msg = "") {
    if (std::abs(expected - actual) > tolerance) {
        std::ostringstream oss;
        oss << "expected " << expected << " but got " << actual;
        if (!msg.empty()) oss << " (" << msg << ")";
        throw AssertionError(oss.str());
    }
}

inline void assertEqual(const std::string& expected, const std::string& actual, const std::string& msg = "") {
    if (expected != actual) {
        std::ostringstream oss;
        oss << "expected \"" << expected << "\" but got \"" << actual << "\"";
        if (!msg.empty()) oss << " (" << msg << ")";
        throw AssertionError(oss.str());
    }
}

inline void assertThrows(std::function<void()> func, const std::string& msg = "") {
    bool threw = false;
    try {
        func();
    } catch (...) {
        threw = true;
    }
    if (!threw) {
        throw AssertionError("expected exception to be thrown" +
                             (msg.empty() ? "" : " (" + msg + ")"));
    }
}

// 性能测量 / Performance measurement
inline double measure(std::function<void()> func, int iterations = 100) {
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        func();
    }
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count() / iterations;
}

// ─── 测试注册宏 / Test registration macros ─────────────────────────────────

#define TEST(name) \
    static void test_##name(); \
    namespace { struct Register_##name { \
        Register_##name() { suki::test::TestSuite::instance().addTest(#name, test_##name); } \
    } register_##name; } \
    static void test_##name()

// 运行所有测试 / Run all tests
inline int runAllTests() {
    return TestSuite::instance().runAll();
}

} // namespace suki::test
