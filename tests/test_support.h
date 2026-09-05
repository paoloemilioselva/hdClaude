// A deliberately tiny assertion harness.
//
// hdClaudeCore has no third-party dependency, and the core-only preset exists
// so its tests run anywhere with just a compiler. Pulling in a test framework
// to check a SHA-256 vector would undo that.

#ifndef HDCLAUDE_TESTS_TEST_SUPPORT_H
#define HDCLAUDE_TESTS_TEST_SUPPORT_H

#include <cmath>
#include <cstdio>
#include <string>

namespace hdclaude_test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void Report(bool passed, const char* expression, const char* file, int line,
                   const std::string& detail = {})
{
    ++g_checks;
    if (passed) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d\n  %s\n", file, line, expression);
    if (!detail.empty()) {
        std::fprintf(stderr, "  %s\n", detail.c_str());
    }
}

inline bool NearlyEqual(double a, double b, double tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

inline int Summarize(const char* suite)
{
    if (g_failures == 0) {
        std::printf("%s: %d checks passed\n", suite, g_checks);
        return 0;
    }
    std::fprintf(stderr, "%s: %d of %d checks FAILED\n", suite, g_failures, g_checks);
    return 1;
}

}  // namespace hdclaude_test

#define CHECK(expr) \
    ::hdclaude_test::Report((expr), #expr, __FILE__, __LINE__)

#define CHECK_NEAR(a, b, tol)                                                  \
    ::hdclaude_test::Report(                                                   \
        ::hdclaude_test::NearlyEqual((a), (b), (tol)), #a " ~= " #b, __FILE__, \
        __LINE__,                                                              \
        "got " + std::to_string(static_cast<double>(a)) + ", expected " +      \
            std::to_string(static_cast<double>(b)) + " +/- " +                 \
            std::to_string(static_cast<double>(tol)))

#define CHECK_EQ(a, b)                                                    \
    ::hdclaude_test::Report((a) == (b), #a " == " #b, __FILE__, __LINE__)

#endif  // HDCLAUDE_TESTS_TEST_SUPPORT_H
