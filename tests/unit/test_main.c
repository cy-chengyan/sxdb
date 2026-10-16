/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Unit test runner.
 *
 * Usage: sxdb-unit-tests [suite ...]
 * With no arguments every suite runs. Exit status is 0 only if every test passed.
 */

#include "sx_test.h"

#include <ftw.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern const sx_test_suite sx_test_suite_common;
extern const sx_test_suite sx_test_suite_log;
extern const sx_test_suite sx_test_suite_mem;
extern const sx_test_suite sx_test_suite_bitpack;
extern const sx_test_suite sx_test_suite_int_codec;
extern const sx_test_suite sx_test_suite_float_codec;
extern const sx_test_suite sx_test_suite_bool_string_codec;

static const sx_test_suite* const all_suites[] = {
    &sx_test_suite_common,    &sx_test_suite_log,         &sx_test_suite_mem,
    &sx_test_suite_bitpack,   &sx_test_suite_int_codec,   &sx_test_suite_float_codec,
    &sx_test_suite_bool_string_codec,
};

static const size_t all_suite_count = sizeof(all_suites) / sizeof(all_suites[0]);

static bool current_test_failed;
static char temp_dir_path[256];

void sx_test_fail(const char* file, int line, const char* format, ...) {
    va_list args;
    fprintf(stderr, "    %s:%d: ", file, line);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    current_test_failed = true;
}

const char* sx_test_temp_dir(void) {
    if (temp_dir_path[0] != '\0') {
        return temp_dir_path;
    }
    const char* base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    int written = snprintf(temp_dir_path, sizeof(temp_dir_path), "%s/sxdb-test-XXXXXX", base);
    if (written < 0 || (size_t)written >= sizeof(temp_dir_path) || mkdtemp(temp_dir_path) == NULL) {
        fprintf(stderr, "cannot create a temporary directory under %s\n", base);
        abort();
    }
    return temp_dir_path;
}

static int remove_entry(const char* path, const struct stat* info, int type, struct FTW* ftw) {
    (void)info;
    (void)type;
    (void)ftw;
    return remove(path);
}

static void remove_temp_dir(void) {
    if (temp_dir_path[0] == '\0') {
        return;
    }
    if (nftw(temp_dir_path, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0) {
        fprintf(stderr, "warning: cannot remove %s\n", temp_dir_path);
    }
    temp_dir_path[0] = '\0';
}

static size_t run_suite(const sx_test_suite* suite) {
    size_t failed = 0;
    printf("[%s]\n", suite->name);
    for (size_t i = 0; i < suite->case_count; i++) {
        const sx_test_case* test = &suite->cases[i];
        current_test_failed = false;
        test->fn();
        remove_temp_dir();
        printf("  %-40s %s\n", test->name, current_test_failed ? "FAIL" : "ok");
        if (current_test_failed) {
            failed++;
        }
    }
    return failed;
}

static const sx_test_suite* find_suite(const char* name) {
    for (size_t i = 0; i < all_suite_count; i++) {
        if (strcmp(all_suites[i]->name, name) == 0) {
            return all_suites[i];
        }
    }
    return NULL;
}

int main(int argc, char** argv) {
    size_t failed = 0;
    if (argc <= 1) {
        for (size_t i = 0; i < all_suite_count; i++) {
            failed += run_suite(all_suites[i]);
        }
    } else {
        for (int i = 1; i < argc; i++) {
            const sx_test_suite* suite = find_suite(argv[i]);
            if (suite == NULL) {
                fprintf(stderr, "unknown test suite: %s\n", argv[i]);
                return 2;
            }
            failed += run_suite(suite);
        }
    }
    if (failed > 0) {
        printf("%zu test(s) failed\n", failed);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
