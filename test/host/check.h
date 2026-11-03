/* Minimal assertion helpers for the host tests. */
#pragma once

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_failures;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                          \
        }                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                                        \
    do {                                                                                                      \
        long long va_ = (long long)(a), vb_ = (long long)(b);                                                 \
        if (va_ != vb_) {                                                                                     \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                    va_, vb_);                                                                                \
            g_failures++;                                                                                     \
        }                                                                                                     \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                    \
    do {                                                                                                         \
        double va_ = (a), vb_ = (b);                                                                             \
        if (fabs(va_ - vb_) > (eps)) {                                                                           \
            fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s ~ %s (%g vs %g)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            g_failures++;                                                                                        \
        }                                                                                                        \
    } while (0)

#define TEST_MAIN_END()                                     \
    do {                                                    \
        if (g_failures) {                                   \
            fprintf(stderr, "%d check(s) failed\n", g_failures); \
            return EXIT_FAILURE;                            \
        }                                                   \
        printf("all checks passed\n");                      \
        return EXIT_SUCCESS;                                \
    } while (0)
