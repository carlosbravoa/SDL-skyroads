// Tiny zero-dependency test harness. Keeps the port free of external test
// frameworks so it builds anywhere a C++17 compiler exists.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace check {

inline int& failures() {
    static int count = 0;
    return count;
}

inline std::string assets_dir() {
    if (const char* env = std::getenv("SKYROADS_ASSETS")) {
        return std::string(env);
    }
    // Default to the bundled asset directory relative to the build tree
    // (build/ -> ../skyroads-assets). Override with SKYROADS_ASSETS.
    return std::string("../skyroads-assets");
}

inline std::string asset(const std::string& name) {
    return assets_dir() + "/" + name;
}

template <typename A, typename B>
void eq(const A& actual, const B& expected, const char* expr, const char* file,
        int line) {
    if (!(actual == expected)) {
        std::ostringstream os;
        os << "FAIL " << file << ":" << line << "  " << expr << "\n";
        std::fputs(os.str().c_str(), stderr);
        failures() += 1;
    }
}

inline void is_true(bool value, const char* expr, const char* file, int line) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s:%d  expected true: %s\n", file, line, expr);
        failures() += 1;
    }
}

} // namespace check

#define CHECK_EQ(actual, expected) \
    ::check::eq((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
#define CHECK_TRUE(value) ::check::is_true((value), #value, __FILE__, __LINE__)

#define CHECK_MAIN_BEGIN() int main() {
#define CHECK_MAIN_END()                                              \
    if (::check::failures() == 0) {                                   \
        std::puts("all checks passed");                              \
        return 0;                                                     \
    }                                                                 \
    std::fprintf(stderr, "%d check(s) failed\n", ::check::failures());\
    return 1;                                                         \
    }
