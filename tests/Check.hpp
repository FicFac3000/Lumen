// Check.hpp — a minimal test harness. Dependency-free on purpose: the build
// should work from a clean checkout with nothing fetched.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace check {

struct Stats {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;
};

inline Stats& stats() {
    static Stats s;
    return s;
}

inline void record(bool ok, const char* expr, const char* file, int line,
                   const std::string& extra = {}) {
    if (ok) {
        ++stats().passed;
        return;
    }
    ++stats().failed;
    std::string msg = std::string(file) + ":" + std::to_string(line) + "  " + expr;
    if (!extra.empty()) msg += "   [" + extra + "]";
    stats().failures.push_back(msg);
}

inline bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

inline int report(const char* suite) {
    const auto& s = stats();
    if (s.failed == 0) {
        std::printf("  %-28s %3d passed\n", suite, s.passed);
        return 0;
    }
    std::printf("  %-28s %3d passed, %d FAILED\n", suite, s.passed, s.failed);
    for (const auto& f : s.failures) std::printf("      %s\n", f.c_str());
    return 1;
}

} // namespace check

#define CHECK(expr) ::check::record((expr), #expr, __FILE__, __LINE__)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        auto _a = (a);                                                              \
        auto _b = (b);                                                              \
        ::check::record(_a == _b, #a " == " #b, __FILE__, __LINE__,                  \
                        std::string("got ") + std::to_string(_a) + ", want " +       \
                            std::to_string(_b));                                    \
    } while (0)

#define CHECK_NEAR(a, b)                                                            \
    do {                                                                            \
        double _a = static_cast<double>(a);                                         \
        double _b = static_cast<double>(b);                                         \
        ::check::record(::check::near(_a, _b), #a " ~= " #b, __FILE__, __LINE__,     \
                        std::string("got ") + std::to_string(_a) + ", want " +       \
                            std::to_string(_b));                                    \
    } while (0)
