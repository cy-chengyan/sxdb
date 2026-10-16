/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * A minimal unit test framework.
 *
 * A test is a function taking no arguments. Checks stop the current test on the first
 * failure by returning from it, so they may only be used directly inside test functions.
 * Tests are grouped into suites; tests/unit/test_main.c lists every suite.
 */

#ifndef SX_TEST_H
#define SX_TEST_H

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef void (*sx_test_fn)(void);

typedef struct sx_test_case {
    const char* name;
    sx_test_fn fn;
} sx_test_case;

typedef struct sx_test_suite {
    const char* name;
    const sx_test_case* cases;
    size_t case_count;
} sx_test_suite;

#define SX_TEST_SUITE(suite_name, case_array)                                                      \
    {                                                                                              \
        .name = (suite_name),                                                                      \
        .cases = (case_array),                                                                     \
        .case_count = sizeof(case_array) / sizeof((case_array)[0]),                                \
    }

/* Records a failure of the running test. Used by the check macros below. */
void sx_test_fail(const char* file, int line, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

/* Returns a path to a fresh, empty temporary directory for the running test. */
const char* sx_test_temp_dir(void);

#define SX_CHECK(cond)                                                                             \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            sx_test_fail(__FILE__, __LINE__, "check failed: %s", #cond);                           \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define SX_CHECK_EQ_U64(actual, expected)                                                          \
    do {                                                                                           \
        uint64_t sx_check_actual = (uint64_t)(actual);                                             \
        uint64_t sx_check_expected = (uint64_t)(expected);                                         \
        if (sx_check_actual != sx_check_expected) {                                                \
            sx_test_fail(__FILE__, __LINE__, "%s == %" PRIu64 ", expected %" PRIu64, #actual,      \
                         sx_check_actual, sx_check_expected);                                      \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define SX_CHECK_EQ_I64(actual, expected)                                                          \
    do {                                                                                           \
        int64_t sx_check_actual = (int64_t)(actual);                                               \
        int64_t sx_check_expected = (int64_t)(expected);                                           \
        if (sx_check_actual != sx_check_expected) {                                                \
            sx_test_fail(__FILE__, __LINE__, "%s == %" PRId64 ", expected %" PRId64, #actual,      \
                         sx_check_actual, sx_check_expected);                                      \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define SX_CHECK_EQ_STR(actual, expected)                                                          \
    do {                                                                                           \
        const char* sx_check_actual = (actual);                                                    \
        const char* sx_check_expected = (expected);                                                \
        if (strcmp(sx_check_actual, sx_check_expected) != 0) {                                     \
            sx_test_fail(__FILE__, __LINE__, "%s == \"%s\", expected \"%s\"", #actual,             \
                         sx_check_actual, sx_check_expected);                                      \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define SX_CHECK_EQ_MEM(actual, expected, size)                                                    \
    do {                                                                                           \
        if (memcmp((actual), (expected), (size)) != 0) {                                           \
            sx_test_fail(__FILE__, __LINE__, "memory differs: %s vs %s", #actual, #expected);      \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#endif /* SX_TEST_H */
