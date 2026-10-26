/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sx_test.h"

#include "common/compiler.h"
#include "os/os.h"
#include "os/os_file.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sx_dir* open_temp_dir(void) {
    sx_dir* dir = NULL;
    if (sx_dir_open_path(sx_test_temp_dir(), &dir) != SX_OK) {
        return NULL;
    }
    return dir;
}

static bool starts_with(const char* text, const char* prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static void test_write_read_size(void) {
    sx_dir* dir = open_temp_dir();
    SX_CHECK(dir != NULL);
    sx_file* file = NULL;
    SX_CHECK(sx_file_open(dir, "data", SX_FILE_CREATE, &file) == SX_OK);
    SX_CHECK(!sx_file_is_direct(file));
    SX_CHECK_EQ_STR(sx_file_name(file), "data");
    SX_CHECK(sx_file_write_at(file, "hello world", 11, 0) == SX_OK);
    SX_CHECK(sx_file_write_at(file, "HELLO", 5, 0) == SX_OK);
    SX_CHECK(sx_file_sync(file) == SX_OK);

    uint64_t size = 0;
    SX_CHECK(sx_file_get_size(file, &size) == SX_OK);
    SX_CHECK_EQ_U64(size, 11);
    char buffer[16] = {0};
    SX_CHECK(sx_file_read_at(file, buffer, 11, 0) == SX_OK);
    SX_CHECK_EQ_STR(buffer, "HELLO world");

    /* Reading past the end is corruption, not a short read. */
    SX_CHECK(sx_file_read_at(file, buffer, 4, 9) == SX_ERR_CORRUPT);
    char message[256];
    sx_os_last_error_message(message, sizeof(message));
    SX_CHECK_EQ_STR(message, "could not read file \"data\": unexpected end of file");
    sx_file_close(file);
    sx_dir_close(dir);
}

static void test_open_errors(void) {
    sx_dir* dir = open_temp_dir();
    SX_CHECK(dir != NULL);
    sx_file* file = NULL;
    SX_CHECK(sx_file_open(dir, "missing", 0, &file) == SX_ERR_NOT_FOUND);
    char message[256];
    sx_os_last_error_message(message, sizeof(message));
    SX_CHECK(starts_with(message, "could not open file \"missing\": "));

    SX_CHECK(sx_file_open(dir, "once", SX_FILE_CREATE, &file) == SX_OK);
    sx_file_close(file);
    SX_CHECK(sx_file_open(dir, "once", SX_FILE_CREATE, &file) == SX_ERR_EXISTS);

    /* A read-only handle refuses writes. */
    SX_CHECK(sx_file_open(dir, "once", 0, &file) == SX_OK);
    SX_CHECK(sx_file_write_at(file, "x", 1, 0) != SX_OK);
    sx_file_close(file);
    sx_dir_close(dir);
}

static void test_names_are_single_components(void) {
    sx_dir* dir = open_temp_dir();
    SX_CHECK(dir != NULL);
    const char* bad_names[] = {"", ".", "..", "a/b", "/abs"};
    for (size_t i = 0; i < SX_ARRAY_SIZE(bad_names); i++) {
        sx_file* file = NULL;
        SX_CHECK(sx_file_open(dir, bad_names[i], SX_FILE_CREATE, &file) == SX_ERR_INVALID_ARG);
        SX_CHECK(sx_dir_create(dir, bad_names[i]) == SX_ERR_INVALID_ARG);
        SX_CHECK(sx_dir_remove_file(dir, bad_names[i]) == SX_ERR_INVALID_ARG);
    }
    char long_name[SX_FILE_NAME_MAX + 2];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    sx_file* file = NULL;
    SX_CHECK(sx_file_open(dir, long_name, SX_FILE_CREATE, &file) == SX_ERR_INVALID_ARG);
    sx_dir_close(dir);
}

static void test_direct_io_alignment(void) {
    sx_dir* dir = open_temp_dir();
    SX_CHECK(dir != NULL);
    sx_file* file = NULL;
    SX_CHECK(sx_file_open(dir, "pages", SX_FILE_CREATE | SX_FILE_DIRECT, &file) == SX_OK);

    const size_t two_pages = 2 * (size_t)SX_FILE_DIRECT_ALIGNMENT;
    void* page = NULL;
    SX_CHECK(posix_memalign(&page, SX_FILE_DIRECT_ALIGNMENT, two_pages) == 0);
    memset(page, 0xAB, two_pages);

    /* Unaligned buffers, lengths, and offsets are refused on every platform. */
    uint8_t* bytes = page;
    SX_CHECK(sx_file_write_at(file, bytes + 1, SX_FILE_DIRECT_ALIGNMENT, 0) == SX_ERR_INVALID_ARG);
    SX_CHECK(sx_file_write_at(file, page, 100, 0) == SX_ERR_INVALID_ARG);
    SX_CHECK(sx_file_write_at(file, page, SX_FILE_DIRECT_ALIGNMENT, 512) == SX_ERR_INVALID_ARG);

    SX_CHECK(sx_file_write_at(file, page, two_pages, 0) == SX_OK);
    SX_CHECK(sx_file_sync(file) == SX_OK);
    memset(page, 0, two_pages);
    SX_CHECK(sx_file_read_at(file, page, SX_FILE_DIRECT_ALIGNMENT, SX_FILE_DIRECT_ALIGNMENT) ==
             SX_OK);
    SX_CHECK_EQ_U64(bytes[0], 0xAB);
    SX_CHECK_EQ_U64(bytes[SX_FILE_DIRECT_ALIGNMENT - 1], 0xAB);
    free(page);
    sx_file_close(file);
    sx_dir_close(dir);
}

static void test_truncate_and_preallocate(void) {
    sx_dir* dir = open_temp_dir();
    SX_CHECK(dir != NULL);
    sx_file* file = NULL;
    SX_CHECK(sx_file_open(dir, "wal", SX_FILE_CREATE, &file) == SX_OK);
    SX_CHECK(sx_file_write_at(file, "abc", 3, 0) == SX_OK);
    SX_CHECK(sx_file_preallocate(file, 65536) == SX_OK);
    uint64_t size = 0;
    SX_CHECK(sx_file_get_size(file, &size) == SX_OK);
    SX_CHECK_EQ_U64(size, 65536);

    /* Existing data is kept and the new bytes read as zeros. */
    uint8_t buffer[8];
    SX_CHECK(sx_file_read_at(file, buffer, 8, 0) == SX_OK);
    SX_CHECK_EQ_MEM(buffer, "abc\0\0\0\0\0", 8);
    SX_CHECK(sx_file_read_at(file, buffer, 8, 65528) == SX_OK);
    SX_CHECK_EQ_MEM(buffer, "\0\0\0\0\0\0\0\0", 8);

    /* Preallocating less than the size changes nothing. */
    SX_CHECK(sx_file_preallocate(file, 100) == SX_OK);
    SX_CHECK(sx_file_get_size(file, &size) == SX_OK);
    SX_CHECK_EQ_U64(size, 65536);

    SX_CHECK(sx_file_truncate(file, 2) == SX_OK);
    SX_CHECK(sx_file_get_size(file, &size) == SX_OK);
    SX_CHECK_EQ_U64(size, 2);
    sx_file_close(file);
    sx_dir_close(dir);
}

typedef struct listing {
    char names[8][32];
    bool is_dir[8];
    size_t count;
} listing;

static sx_err collect_entry(void* context, const char* name, bool is_dir) {
    listing* list = context;
    if (list->count == SX_ARRAY_SIZE(list->names)) {
        return SX_ERR_LIMIT;
    }
    snprintf(list->names[list->count], sizeof(list->names[0]), "%s", name);
    list->is_dir[list->count] = is_dir;
    list->count++;
    return SX_OK;
}

static int find_entry(const listing* list, const char* name) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->names[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void test_rename_remove_list(void) {
    sx_dir* dir = open_temp_dir();
    SX_CHECK(dir != NULL);
    sx_file* file = NULL;
    SX_CHECK(sx_file_open(dir, "a", SX_FILE_CREATE, &file) == SX_OK);
    SX_CHECK(sx_file_write_at(file, "A", 1, 0) == SX_OK);
    sx_file_close(file);
    SX_CHECK(sx_file_open(dir, "b", SX_FILE_CREATE, &file) == SX_OK);
    SX_CHECK(sx_file_write_at(file, "B", 1, 0) == SX_OK);
    sx_file_close(file);
    SX_CHECK(sx_dir_create(dir, "sub") == SX_OK);
    SX_CHECK(sx_dir_create(dir, "sub") == SX_ERR_EXISTS);

    /* Renaming replaces the target. */
    SX_CHECK(sx_dir_rename(dir, "a", "b") == SX_OK);
    SX_CHECK(sx_dir_sync(dir) == SX_OK);
    char content[4] = {0};
    size_t size = 0;
    SX_CHECK(sx_dir_read_file(dir, "b", content, sizeof(content), &size) == SX_OK);
    SX_CHECK_EQ_U64(size, 1);
    SX_CHECK_EQ_STR(content, "A");

    listing list = {0};
    SX_CHECK(sx_dir_list(dir, collect_entry, &list) == SX_OK);
    SX_CHECK_EQ_U64(list.count, 2);
    int b = find_entry(&list, "b");
    int sub = find_entry(&list, "sub");
    SX_CHECK(b >= 0 && !list.is_dir[b]);
    SX_CHECK(sub >= 0 && list.is_dir[sub]);

    SX_CHECK(sx_dir_remove_file(dir, "b") == SX_OK);
    SX_CHECK(sx_dir_remove_file(dir, "b") == SX_ERR_NOT_FOUND);
    SX_CHECK(sx_file_open(dir, "b", 0, &file) == SX_ERR_NOT_FOUND);

    sx_dir* child = NULL;
    SX_CHECK(sx_dir_open(dir, "sub", &child) == SX_OK);
    SX_CHECK(strcmp(sx_dir_path(child) + strlen(sx_dir_path(child)) - 4, "/sub") == 0);
    sx_dir_close(child);
    sx_dir_close(dir);
}

static void test_replace_and_read_file(void) {
    sx_dir* dir = open_temp_dir();
    SX_CHECK(dir != NULL);
    SX_CHECK(sx_dir_replace_file(dir, "CURRENT", "first\n", 6) == SX_OK);
    SX_CHECK(sx_dir_replace_file(dir, "CURRENT", "second\n", 7) == SX_OK);

    char content[16] = {0};
    size_t size = 0;
    SX_CHECK(sx_dir_read_file(dir, "CURRENT", content, sizeof(content), &size) == SX_OK);
    SX_CHECK_EQ_U64(size, 7);
    SX_CHECK_EQ_STR(content, "second\n");
    SX_CHECK(sx_dir_read_file(dir, "CURRENT", content, 3, &size) == SX_ERR_LIMIT);

    /* No temporary file is left behind. */
    listing list = {0};
    SX_CHECK(sx_dir_list(dir, collect_entry, &list) == SX_OK);
    SX_CHECK_EQ_U64(list.count, 1);
    SX_CHECK_EQ_STR(list.names[0], "CURRENT");
    sx_dir_close(dir);
}

static void test_lock_is_exclusive(void) {
    sx_dir* dir = open_temp_dir();
    SX_CHECK(dir != NULL);
    sx_file* first = NULL;
    sx_file* second = NULL;
    SX_CHECK(sx_file_open(dir, "LOCK", SX_FILE_CREATE, &first) == SX_OK);
    SX_CHECK(sx_file_open(dir, "LOCK", SX_FILE_WRITE, &second) == SX_OK);
    SX_CHECK(sx_file_try_lock(first) == SX_OK);
    SX_CHECK(sx_file_try_lock(second) == SX_ERR_BUSY);
    sx_file_close(first);
    SX_CHECK(sx_file_try_lock(second) == SX_OK);
    sx_file_close(second);
    sx_dir_close(dir);
}

static void test_create_path_and_info(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/data/", sx_test_temp_dir());
    SX_CHECK(sx_dir_create_path(path) == SX_OK);
    SX_CHECK(sx_dir_create_path(path) == SX_ERR_EXISTS);
    sx_dir* dir = NULL;
    SX_CHECK(sx_dir_open_path(path, &dir) == SX_OK);
    sx_dir_info info;
    SX_CHECK(sx_dir_get_info(dir, &info) == SX_OK);
    SX_CHECK_EQ_U64(info.owner_uid, sx_os_effective_uid());
    SX_CHECK_EQ_U64(info.mode & 0077, 0);
    SX_CHECK_EQ_U64(info.mode & 0700, 0700);
    sx_dir_close(dir);

    snprintf(path, sizeof(path), "%s/missing/child", sx_test_temp_dir());
    SX_CHECK(sx_dir_create_path(path) == SX_ERR_NOT_FOUND);
}

static void test_random_bytes(void) {
    uint8_t first[300];
    uint8_t second[300];
    SX_CHECK(sx_os_random_bytes(first, sizeof(first)) == SX_OK);
    SX_CHECK(sx_os_random_bytes(second, sizeof(second)) == SX_OK);
    SX_CHECK(memcmp(first, second, sizeof(first)) != 0);
}

static const sx_test_case os_file_cases[] = {
    {"write_read_size", test_write_read_size},
    {"open_errors", test_open_errors},
    {"names_are_single_components", test_names_are_single_components},
    {"direct_io_alignment", test_direct_io_alignment},
    {"truncate_and_preallocate", test_truncate_and_preallocate},
    {"rename_remove_list", test_rename_remove_list},
    {"replace_and_read_file", test_replace_and_read_file},
    {"lock_is_exclusive", test_lock_is_exclusive},
    {"create_path_and_info", test_create_path_and_info},
    {"random_bytes", test_random_bytes},
};

const sx_test_suite sx_test_suite_os_file = SX_TEST_SUITE("os_file", os_file_cases);
