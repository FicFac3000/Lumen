// Check.hpp — a minimal test harness. Dependency-free on purpose: the build
// should work from a clean checkout with nothing fetched.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>
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

// Renders a value for a failure message. Arithmetic types print; containers
// are joined; anything else degrades to a placeholder rather than failing to
// compile, so CHECK_EQ works on any comparable type.
template <typename T>
std::string describe(const T& v);

template <typename T>
std::string describe(const std::vector<T>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += ", ";
        o += describe(v[i]);
    }
    return o + "]";
}

inline std::string describe(const std::string& v) { return "\"" + v + "\""; }

template <typename T>
std::string describe(const T& v) {
    if constexpr (std::is_arithmetic_v<T>) return std::to_string(v);
    else if constexpr (std::is_enum_v<T>)
        return std::to_string(static_cast<long long>(v));
    else return "<value>";
}

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

// Variadic so a braced initialiser inside the expression -- Cursor{0, 0} --
// is not mistaken for a second macro argument.
#define CHECK(...) ::check::record((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        auto _a = (a);                                                              \
        auto _b = (b);                                                              \
        ::check::record(_a == _b, #a " == " #b, __FILE__, __LINE__,                  \
                        std::string("got ") + ::check::describe(_a) + ", want " +    \
                            ::check::describe(_b));                                  \
    } while (0)

#define CHECK_NEAR(a, b)                                                            \
    do {                                                                            \
        double _a = static_cast<double>(a);                                         \
        double _b = static_cast<double>(b);                                         \
        ::check::record(::check::near(_a, _b), #a " ~= " #b, __FILE__, __LINE__,     \
                        std::string("got ") + std::to_string(_a) + ", want " +       \
                            std::to_string(_b));                                    \
    } while (0)
