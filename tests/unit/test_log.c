/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sx_test.h"

#include "common/log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#define LINE_TIMESTAMP_LENGTH 24u

/* Reads a whole file into a NUL-terminated buffer owned by the caller. */
static char* read_file(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* data = malloc((size_t)length + 1u);
    size_t read = fread(data, 1, (size_t)length, file);
    fclose(file);
    data[read] = '\0';
    if (size != NULL) {
        *size = read;
    }
    return data;
}

static size_t count_lines(const char* text) {
    size_t lines = 0;
    for (const char* p = text; *p != '\0'; p++) {
        if (*p == '\n') {
            lines++;
        }
    }
    return lines;
}

static void current_log_path(char* path, size_t size) {
    snprintf(path, size, "%s/sxdb.log", sx_test_temp_dir());
}

static sx_log_config file_config(sx_log_level level, uint64_t file_size, uint64_t total_size) {
    sx_log_config config = {
        .level = level,
        .output = SX_LOG_OUTPUT_FILE,
        .dir = sx_test_temp_dir(),
        .name = "sxdb",
        .file_size = file_size,
        .total_size = total_size,
    };
    return config;
}

static void restore_defaults(void) {
    sx_log_close();
    sx_log_set_level(SX_LOG_LEVEL_WARN);
}

static void test_line_format_and_escaping(void) {
    sx_log_config config = file_config(SX_LOG_LEVEL_INFO, 1u << 20, 4u << 20);
    SX_CHECK(sx_log_open(&config) == SX_OK);
    SX_LOG_WARN("test", "hello\nworld\t%d", 42);
    SX_LOG_INFO("test", "bell\x01!");
    SX_LOG_DEBUG("test", "not written at info level");
    restore_defaults();

    char path[512];
    current_log_path(path, sizeof(path));
    char* text = read_file(path, NULL);
    SX_CHECK(text != NULL);
    size_t lines = count_lines(text);
    char first[256] = {0};
    char second[256] = {0};
    sscanf(text, "%255[^\n]\n%255[^\n]", first, second);
    free(text);

    SX_CHECK_EQ_U64(lines, 2);
    SX_CHECK(first[4] == '-' && first[10] == 'T' && first[23] == 'Z');
    SX_CHECK_EQ_STR(first + LINE_TIMESTAMP_LENGTH, " WARN  [test] hello\\nworld\\t42");
    SX_CHECK_EQ_STR(second + LINE_TIMESTAMP_LENGTH, " INFO  [test] bell\\x01!");
}

static void test_long_message_is_truncated(void) {
    sx_log_config config = file_config(SX_LOG_LEVEL_WARN, 1u << 20, 4u << 20);
    SX_CHECK(sx_log_open(&config) == SX_OK);
    char message[5000];
    memset(message, 'a', sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
    SX_LOG_WARN("test", "%s", message);
    restore_defaults();

    char path[512];
    current_log_path(path, sizeof(path));
    size_t size = 0;
    char* text = read_file(path, &size);
    SX_CHECK(text != NULL);
    bool ends_with_ellipsis = size >= 4 && strcmp(text + size - 4, "...\n") == 0;
    free(text);
    SX_CHECK(size <= 1024);
    SX_CHECK(ends_with_ellipsis);
}

static void test_rotation_respects_size_limits(void) {
    const uint64_t file_size = 300;
    const uint64_t total_size = 1000;
    sx_log_config config = file_config(SX_LOG_LEVEL_WARN, file_size, total_size);
    SX_CHECK(sx_log_open(&config) == SX_OK);
    for (int i = 0; i < 200; i++) {
        SX_LOG_WARN("rotate", "line number %d with some padding text", i);
    }
    restore_defaults();

    DIR* dir = opendir(sx_test_temp_dir());
    SX_CHECK(dir != NULL);
    uint64_t total = 0;
    size_t rotated = 0;
    bool has_current = false;
    bool file_too_large = false;
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char path[512];
        struct stat info;
        snprintf(path, sizeof(path), "%s/%s", sx_test_temp_dir(), entry->d_name);
        if (stat(path, &info) != 0) {
            continue;
        }
        total += (uint64_t)info.st_size;
        if ((uint64_t)info.st_size > file_size) {
            file_too_large = true;
        }
        if (strcmp(entry->d_name, "sxdb.log") == 0) {
            has_current = true;
        } else if (strncmp(entry->d_name, "sxdb-", 5) == 0) {
            rotated++;
        }
    }
    closedir(dir);

    SX_CHECK(has_current);
    SX_CHECK(rotated >= 1);
    SX_CHECK(!file_too_large);
    SX_CHECK(total <= total_size);
}

static void test_invalid_configuration_is_rejected(void) {
    sx_log_config config = file_config(SX_LOG_LEVEL_WARN, 1000, 999);
    SX_CHECK(sx_log_open(&config) == SX_ERR_INVALID_ARG);

    config = file_config(SX_LOG_LEVEL_WARN, 1000, 4000);
    config.name = "bad/name";
    SX_CHECK(sx_log_open(&config) == SX_ERR_INVALID_ARG);

    config = file_config(SX_LOG_LEVEL_WARN, 1000, 4000);
    config.dir = NULL;
    SX_CHECK(sx_log_open(&config) == SX_ERR_INVALID_ARG);

    config = file_config(SX_LOG_LEVEL_WARN, 0, 4000);
    SX_CHECK(sx_log_open(&config) == SX_ERR_INVALID_ARG);
}

static void test_dropped_lines_are_counted(void) {
    const int total_lines = 20000;
    sx_log_config config = file_config(SX_LOG_LEVEL_DEBUG, UINT64_C(1) << 30, UINT64_C(1) << 31);
    SX_CHECK(sx_log_open(&config) == SX_OK);
    uint64_t dropped_before = sx_log_dropped_count();
    for (int i = 0; i < total_lines; i++) {
        SX_LOG_INFO("flood", "message %d", i);
    }
    restore_defaults();
    uint64_t dropped = sx_log_dropped_count() - dropped_before;

    char path[512];
    current_log_path(path, sizeof(path));
    char* text = read_file(path, NULL);
    SX_CHECK(text != NULL);
    size_t written = count_lines(text);
    free(text);
    SX_CHECK_EQ_U64(written + dropped, (uint64_t)total_lines);
}

static void test_levels(void) {
    sx_log_level level = SX_LOG_LEVEL_ERROR;
    SX_CHECK(sx_log_parse_level("debug", &level) == SX_OK);
    SX_CHECK(level == SX_LOG_LEVEL_DEBUG);
    SX_CHECK(sx_log_parse_level("WARN", &level) == SX_ERR_INVALID_ARG);
    SX_CHECK_EQ_STR(sx_log_level_name(SX_LOG_LEVEL_INFO), "INFO");

    sx_log_set_level(SX_LOG_LEVEL_ERROR);
    SX_CHECK(sx_log_enabled(SX_LOG_LEVEL_ERROR));
    SX_CHECK(!sx_log_enabled(SX_LOG_LEVEL_WARN));
    restore_defaults();
    SX_CHECK(sx_log_enabled(SX_LOG_LEVEL_WARN));
    SX_CHECK(!sx_log_enabled(SX_LOG_LEVEL_INFO));
}

static const sx_test_case cases[] = {
    {"line_format_and_escaping", test_line_format_and_escaping},
    {"long_message_truncated", test_long_message_is_truncated},
    {"rotation_size_limits", test_rotation_respects_size_limits},
    {"invalid_configuration", test_invalid_configuration_is_rejected},
    {"dropped_lines_counted", test_dropped_lines_are_counted},
    {"levels", test_levels},
};

const sx_test_suite sx_test_suite_log = SX_TEST_SUITE("log", cases);
