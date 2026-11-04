// Tiny assertion helpers for the runtime tests (no test framework dependency,
// so the same binaries run natively and in Node).
#pragma once

#include <cstdint>
#include <cstdio>

namespace lgp_test {
inline int g_failures = 0;
}

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++lgp_test::g_failures;                                       \
        }                                                                 \
    } while (0)

#define CHECK_EQ(actual, expected)                                                                    \
    do {                                                                                              \
        const auto a_ = (actual);                                                                     \
        const auto e_ = (expected);                                                                   \
        if (!(a_ == e_)) {                                                                            \
            std::printf("  FAIL %s:%d: %s == 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #actual, \
                        static_cast<unsigned long long>(a_), static_cast<unsigned long long>(e_));    \
            ++lgp_test::g_failures;                                                                   \
        }                                                                                             \
    } while (0)

#define RUN(test)                    \
    do {                             \
        std::printf("[%s]\n", #test); \
        test();                      \
    } while (0)

inline int finish() {
    if (lgp_test::g_failures != 0) {
        std::printf("%d check(s) failed\n", lgp_test::g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
