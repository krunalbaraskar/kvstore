#pragma once

// A minimal test harness: CHECK records a failure and keeps going, so one
// run reports every broken expectation.

#include <cstdio>

inline int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #cond);                                                 \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

inline int finish(const char* name) {
    if (g_failures == 0) {
        std::printf("%s: all checks passed\n", name);
        return 0;
    }
    std::printf("%s: %d check(s) failed\n", name, g_failures);
    return 1;
}
