/* gv_test.h — a very small self-registering test framework.
 *
 * Tests declare themselves with GV_TEST(name) { ... } and are collected at
 * load time via constructor attributes, so adding a test file to the build is
 * the only wiring required.
 */
#ifndef GV_TEST_H
#define GV_TEST_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*gv_test_fn)(void);

void gv_test_register(const char *name, gv_test_fn fn);
void gv_test_fail(const char *file, int line, const char *fmt, ...);
/* Counts a successful check; used for the "N assertions" summary line. */
void gv_test_pass_check(void);

#define GV_TEST(name)                                                              \
    static void gv_test_body_##name(void);                                         \
    __attribute__((constructor)) static void gv_test_reg_##name(void)              \
    {                                                                              \
        gv_test_register(#name, gv_test_body_##name);                              \
    }                                                                              \
    static void gv_test_body_##name(void)

#define GV_CHECK(cond)                                                             \
    do {                                                                           \
        if (!(cond)) {                                                             \
            gv_test_fail(__FILE__, __LINE__, "expected: %s", #cond);               \
        } else {                                                                   \
            gv_test_pass_check();                                                  \
        }                                                                          \
    } while (0)

#define GV_CHECKF(cond, ...)                                                       \
    do {                                                                           \
        if (!(cond)) {                                                             \
            gv_test_fail(__FILE__, __LINE__, __VA_ARGS__);                         \
        } else {                                                                   \
            gv_test_pass_check();                                                  \
        }                                                                          \
    } while (0)

#define GV_CHECK_EQ_I(a, b)                                                        \
    do {                                                                           \
        long long ga_ = (long long)(a), gb_ = (long long)(b);                      \
        if (ga_ != gb_) {                                                          \
            gv_test_fail(__FILE__, __LINE__, "%s == %s (got %lld, want %lld)",     \
                         #a, #b, ga_, gb_);                                        \
        } else {                                                                   \
            gv_test_pass_check();                                                  \
        }                                                                          \
    } while (0)

#define GV_CHECK_NEAR(a, b, eps)                                                   \
    do {                                                                           \
        double ga_ = (double)(a), gb_ = (double)(b), ge_ = (double)(eps);          \
        if (!(fabs(ga_ - gb_) <= ge_)) {                                           \
            gv_test_fail(__FILE__, __LINE__, "%s ~= %s (got %g, want %g +/- %g)",  \
                         #a, #b, ga_, gb_, ge_);                                   \
        } else {                                                                   \
            gv_test_pass_check();                                                  \
        }                                                                          \
    } while (0)

#define GV_CHECK_FINITE(v)                                                         \
    do {                                                                           \
        double gv_ = (double)(v);                                                  \
        if (!isfinite(gv_)) {                                                      \
            gv_test_fail(__FILE__, __LINE__, "%s is not finite (%g)", #v, gv_);    \
        } else {                                                                   \
            gv_test_pass_check();                                                  \
        }                                                                          \
    } while (0)

#endif /* GV_TEST_H */
