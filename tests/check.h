#pragma once
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// Minimal test harness, the project has no test framework dependency

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& testRegistry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& testFailures() {
    static int failures = 0;
    return failures;
}

struct TestRegistrar {
    TestRegistrar(const char* name, std::function<void()> fn) { testRegistry().push_back({ name, fn }); }
};

#define TEST(name)                                                   \
    static void test_##name();                                       \
    static TestRegistrar registrar_##name(#name, test_##name);       \
    static void test_##name()

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
            testFailures()++;                                                                \
        }                                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                          \
    do {                                                                                               \
        double _a = (a), _b = (b);                                                                     \
        if (!(std::fabs(_a - _b) <= (tol))) {                                                          \
            std::printf("  FAILED %s:%d: %s = %f, expected %f +/- %f\n", __FILE__, __LINE__, #a, _a, _b, (double)(tol)); \
            testFailures()++;                                                                          \
        }                                                                                              \
    } while (0)

inline int runTests() {
    for (auto& t : testRegistry()) {
        int before = testFailures();
        t.fn();
        std::printf("%s %s\n", (testFailures() == before) ? "ok  " : "FAIL", t.name);
    }
    std::printf("%d failure(s)\n", testFailures());
    return testFailures() ? 1 : 0;
}
