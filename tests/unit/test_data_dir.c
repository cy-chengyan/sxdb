/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sx_test.h"

#include "common/compiler.h"
#include "common/crc32c.h"
#include "os/os.h"
#include "os/os_fault.h"
#include "os/os_file.h"
#include "storage/data_dir.h"
#include "storage/format.h"

#include "sxdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------------------- */
/* FORMAT                                                                                   */
/* ---------------------------------------------------------------------------------------- */

static void test_format_round_trip(void) {
    sx_format format;
    SX_CHECK(sx_format_init(&format) == SX_OK);
    SX_CHECK_EQ_STR(format.created_by, SXDB_VERSION_STRING);
    format.feature_count = 2;
    format.features[0] = (sx_format_feature){SX_FEATURE_RO_COMPAT, "example_feature", "0.3.0"};
    format.features[1] = (sx_format_feature){SX_FEATURE_INCOMPAT, "wide_ids", "1.2.0-rc1"};

    char text[SX_FORMAT_FILE_MAX];
    size_t size = 0;
    SX_CHECK(sx_format_encode(&format, text, sizeof(text), &size) == SX_OK);
    SX_CHECK(sx_format_encode(&format, text, 40, &size) == SX_ERR_LIMIT);
    SX_CHECK(sx_format_encode(&format, text, sizeof(text), &size) == SX_OK);

    char id[33];
    sx_format_directory_id_text(&format, id);
    char expected[512];
    int prefix = snprintf(expected, sizeof(expected),
                          "sxdb-format 1\ndirectory_id %s\ncreated_by %s\n"
                          "feature ro_compat example_feature 0.3.0\n"
                          "feature incompat wide_ids 1.2.0-rc1\n",
                          id, SXDB_VERSION_STRING);
    snprintf(expected + prefix, sizeof(expected) - (size_t)prefix, "crc32c %08x\n",
             (unsigned)sx_crc32c(0, expected, (size_t)prefix));
    SX_CHECK_EQ_U64(size, strlen(expected));
    SX_CHECK_EQ_MEM(text, expected, size);

    sx_format decoded;
    SX_CHECK(sx_format_decode(text, size, &decoded) == SX_OK);
    SX_CHECK_EQ_MEM(decoded.directory_id, format.directory_id, 16);
    SX_CHECK_EQ_STR(decoded.created_by, SXDB_VERSION_STRING);
    SX_CHECK_EQ_U64(decoded.feature_count, 2);
    SX_CHECK_EQ_STR(decoded.features[1].name, "wide_ids");
    SX_CHECK_EQ_U64(decoded.features[1].feature_class, SX_FEATURE_INCOMPAT);
    SX_CHECK_EQ_STR(decoded.features[1].version, "1.2.0-rc1");

    /* Two new formats get different ids. */
    sx_format other;
    SX_CHECK(sx_format_init(&other) == SX_OK);
    SX_CHECK(memcmp(other.directory_id, format.directory_id, 16) != 0);
}

/* Builds a FORMAT text from lines, appending a correct CRC line unless `crc_line` is set. */
static size_t build_text(char* out, size_t capacity, const char* body, const char* crc_line) {
    int length = snprintf(out, capacity, "%s", body);
    if (crc_line != NULL) {
        length += snprintf(out + length, capacity - (size_t)length, "%s", crc_line);
    } else {
        length += snprintf(out + length, capacity - (size_t)length, "crc32c %08x\n",
                           (unsigned)sx_crc32c(0, out, (size_t)length));
    }
    return (size_t)length;
}

static void test_format_rejects_malformed_text(void) {
    static const char* const id_line = "directory_id 00112233445566778899aabbccddeeff\n";
    char good_body[256];
    snprintf(good_body, sizeof(good_body), "sxdb-format 1\n%screated_by 0.1.0\n", id_line);
    char text[512];
    sx_format format;
    size_t size = build_text(text, sizeof(text), good_body, NULL);
    SX_CHECK(sx_format_decode(text, size, &format) == SX_OK);
    SX_CHECK_EQ_U64(format.directory_id[15], 0xff);

    static const char* const bad_bodies[] = {
        "sxdb-format 2\ndirectory_id 00112233445566778899aabbccddeeff\ncreated_by 0.1.0\n",
        "sxdb-format 1\ndirectory_id 00112233445566778899AABBCCDDEEFF\ncreated_by 0.1.0\n",
        "sxdb-format 1\ndirectory_id 0011223344556677\ncreated_by 0.1.0\n",
        "sxdb-format 1\ncreated_by 0.1.0\n",
        "sxdb-format 1\ndirectory_id 00112233445566778899aabbccddeeff\ncreated_by 0.1 .0\n",
        "sxdb-format 1\ndirectory_id 00112233445566778899aabbccddeeff\ncreated_by 0.1.0\n"
        "feature sometimes x 1.0\n",
        "sxdb-format 1\ndirectory_id 00112233445566778899aabbccddeeff\ncreated_by 0.1.0\n"
        "feature incompat Bad-Name 1.0\n",
        "sxdb-format 1\ndirectory_id 00112233445566778899aabbccddeeff\ncreated_by 0.1.0\n"
        "feature incompat name\n",
        "sxdb-format 1\ndirectory_id 00112233445566778899aabbccddeeff\ncreated_by 0.1.0\n"
        "unknown_key value\n",
        "sxdb-format 1\r\ndirectory_id 00112233445566778899aabbccddeeff\ncreated_by 0.1.0\n",
    };
    for (size_t i = 0; i < SX_ARRAY_SIZE(bad_bodies); i++) {
        size = build_text(text, sizeof(text), bad_bodies[i], NULL);
        SX_CHECK(sx_format_decode(text, size, &format) == SX_ERR_CORRUPT);
    }

    /* A wrong CRC, a missing CRC line, a missing final LF, and trailing bytes. */
    size = build_text(text, sizeof(text), good_body, "crc32c 00000000\n");
    SX_CHECK(sx_format_decode(text, size, &format) == SX_ERR_CORRUPT);
    size = build_text(text, sizeof(text), good_body, "");
    SX_CHECK(sx_format_decode(text, size, &format) == SX_ERR_CORRUPT);
    size = build_text(text, sizeof(text), good_body, NULL);
    SX_CHECK(sx_format_decode(text, size - 1, &format) == SX_ERR_CORRUPT);
    text[size] = 'x';
    SX_CHECK(sx_format_decode(text, size + 1, &format) == SX_ERR_CORRUPT);

    /* Any single flipped bit is caught. */
    size = build_text(text, sizeof(text), good_body, NULL);
    for (size_t i = 0; i < size; i++) {
        for (int bit = 0; bit < 8; bit++) {
            text[i] ^= (char)(1 << bit);
            SX_CHECK(sx_format_decode(text, size, &format) == SX_ERR_CORRUPT);
            text[i] ^= (char)(1 << bit);
        }
    }
}

static void test_format_feature_checks(void) {
    sx_format format;
    SX_CHECK(sx_format_init(&format) == SX_OK);
    const sx_format_feature* blocking = NULL;
    SX_CHECK(sx_format_check(&format, true, &blocking) == SX_OK);

    format.feature_count = 1;
    format.features[0] = (sx_format_feature){SX_FEATURE_COMPAT, "future_hint", "9.0.0"};
    SX_CHECK(sx_format_check(&format, true, &blocking) == SX_OK);

    format.features[0].feature_class = SX_FEATURE_RO_COMPAT;
    SX_CHECK(sx_format_check(&format, false, &blocking) == SX_OK);
    SX_CHECK(sx_format_check(&format, true, &blocking) == SX_ERR_UNSUPPORTED);
    SX_CHECK_EQ_STR(blocking->version, "9.0.0");

    format.features[0].feature_class = SX_FEATURE_INCOMPAT;
    SX_CHECK(sx_format_check(&format, false, &blocking) == SX_ERR_UNSUPPORTED);
    SX_CHECK_EQ_STR(blocking->name, "future_hint");
}

/* ---------------------------------------------------------------------------------------- */
/* Opening data directories                                                                 */
/* ---------------------------------------------------------------------------------------- */

static void temp_path(char* path, size_t capacity, const char* name) {
    snprintf(path, capacity, "%s/%s", sx_test_temp_dir(), name);
}

static void test_initializes_and_reopens(void) {
    char path[512];
    temp_path(path, sizeof(path), "data");
    char message[SX_DATA_DIR_MESSAGE_SIZE];
    sx_data_dir data_dir;

    /* The server refuses a directory that does not exist yet. */
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_EXISTING, &data_dir, message, sizeof(message)) ==
             SX_ERR_NOT_FOUND);
    SX_CHECK(strstr(message, "sxdb init") != NULL);

    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_INITIALIZE, &data_dir, message, sizeof(message)) ==
             SX_OK);
    SX_CHECK(data_dir.initialized);
    uint8_t id[16];
    memcpy(id, data_dir.format.directory_id, sizeof(id));
    sx_dir_info info;
    SX_CHECK(sx_dir_get_info(data_dir.dir, &info) == SX_OK);
    SX_CHECK_EQ_U64(info.mode & 0777, 0700);
    sx_data_dir_close(&data_dir);

    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_EXISTING, &data_dir, message, sizeof(message)) ==
             SX_OK);
    SX_CHECK(!data_dir.initialized);
    SX_CHECK_EQ_MEM(data_dir.format.directory_id, id, sizeof(id));
    sx_data_dir_close(&data_dir);
}

static void test_lock_is_held(void) {
    char path[512];
    temp_path(path, sizeof(path), "data");
    char message[SX_DATA_DIR_MESSAGE_SIZE];
    sx_data_dir first;
    sx_data_dir second;
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_INITIALIZE, &first, message, sizeof(message)) ==
             SX_OK);
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_EXISTING, &second, message, sizeof(message)) ==
             SX_ERR_BUSY);
    char pid[32];
    snprintf(pid, sizeof(pid), "process %llu", (unsigned long long)sx_os_process_id());
    SX_CHECK(strstr(message, pid) != NULL);
    sx_data_dir_close(&first);
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_EXISTING, &second, message, sizeof(message)) ==
             SX_OK);
    sx_data_dir_close(&second);
}

static void test_refuses_foreign_directory(void) {
    char path[512];
    temp_path(path, sizeof(path), "photos");
    SX_CHECK(sx_dir_create_path(path) == SX_OK);
    sx_dir* dir = NULL;
    SX_CHECK(sx_dir_open_path(path, &dir) == SX_OK);
    SX_CHECK(sx_dir_replace_file(dir, "holiday.jpg", "jpeg", 4) == SX_OK);

    char message[SX_DATA_DIR_MESSAGE_SIZE];
    sx_data_dir data_dir;
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_INITIALIZE, &data_dir, message, sizeof(message)) ==
             SX_ERR_INVALID_ARG);
    SX_CHECK(strstr(message, "not an sxdb data directory") != NULL);
    SX_CHECK(strstr(message, "holiday.jpg") != NULL);
    /* Nothing was written into it. */
    sx_file* file = NULL;
    SX_CHECK(sx_file_open(dir, "LOCK", 0, &file) == SX_ERR_NOT_FOUND);
    sx_dir_close(dir);
}

static void test_checks_permissions(void) {
    char path[512];
    temp_path(path, sizeof(path), "data");
    char message[SX_DATA_DIR_MESSAGE_SIZE];
    sx_data_dir data_dir;
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_INITIALIZE, &data_dir, message, sizeof(message)) ==
             SX_OK);
    sx_data_dir_close(&data_dir);

    SX_CHECK(chmod(path, 0755) == 0);
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_EXISTING, &data_dir, message, sizeof(message)) ==
             SX_ERR_PERMISSION);
    SX_CHECK(strstr(message, "0755") != NULL);
    SX_CHECK(chmod(path, 0750) == 0);
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_EXISTING, &data_dir, message, sizeof(message)) ==
             SX_OK);
    sx_data_dir_close(&data_dir);
    SX_CHECK(chmod(path, 0700) == 0);
}

static void test_format_problems_refuse_opening(void) {
    char path[512];
    temp_path(path, sizeof(path), "data");
    char message[SX_DATA_DIR_MESSAGE_SIZE];
    sx_data_dir data_dir;
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_INITIALIZE, &data_dir, message, sizeof(message)) ==
             SX_OK);

    /* A feature from a newer release. */
    sx_format newer = data_dir.format;
    newer.feature_count = 1;
    newer.features[0] = (sx_format_feature){SX_FEATURE_INCOMPAT, "time_travel", "7.1.0"};
    SX_CHECK(sx_format_write(data_dir.dir, &newer) == SX_OK);
    sx_dir* dir = NULL;
    SX_CHECK(sx_dir_open_path(path, &dir) == SX_OK);
    sx_data_dir_close(&data_dir);
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_EXISTING, &data_dir, message, sizeof(message)) ==
             SX_ERR_UNSUPPORTED);
    SX_CHECK(strstr(message, "requires sxdb >= 7.1.0") != NULL);

    /* A damaged FORMAT file. */
    SX_CHECK(sx_dir_replace_file(dir, "FORMAT", "sxdb-format 1\n", 14) == SX_OK);
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_EXISTING, &data_dir, message, sizeof(message)) ==
             SX_ERR_CORRUPT);
    SX_CHECK(strstr(message, "damaged") != NULL);
    sx_dir_close(dir);
}

static void test_removes_temporary_leftovers(void) {
    char path[512];
    temp_path(path, sizeof(path), "data");
    char message[SX_DATA_DIR_MESSAGE_SIZE];
    sx_data_dir data_dir;
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_INITIALIZE, &data_dir, message, sizeof(message)) ==
             SX_OK);
    sx_file* file = NULL;
    SX_CHECK(sx_file_open(data_dir.dir, "CURRENT.tmp", SX_FILE_CREATE, &file) == SX_OK);
    sx_file_close(file);
    sx_data_dir_close(&data_dir);

    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_EXISTING, &data_dir, message, sizeof(message)) ==
             SX_OK);
    SX_CHECK(sx_file_open(data_dir.dir, "CURRENT.tmp", 0, &file) == SX_ERR_NOT_FOUND);
    sx_data_dir_close(&data_dir);
}

/*
 * A crash with a power loss at every point of initialization leaves a directory that the
 * next initialization completes.
 */
static void test_initialization_survives_crashes(void) {
    char message[SX_DATA_DIR_MESSAGE_SIZE];
    sx_data_dir data_dir;
    char path[512];
    char backup[512];
    temp_path(path, sizeof(path), "count");
    sx_fault_plan plan = {0};
    SX_CHECK(sx_fault_arm(&plan) == SX_OK);
    SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_INITIALIZE, &data_dir, message, sizeof(message)) ==
             SX_OK);
    sx_data_dir_close(&data_dir);
    uint64_t points = sx_fault_point_count();
    sx_fault_disarm();
    SX_CHECK(points >= 5);

    for (uint64_t point = 1; point <= points; point++) {
        char name[32];
        snprintf(name, sizeof(name), "data%llu", (unsigned long long)point);
        temp_path(path, sizeof(path), name);
        snprintf(name, sizeof(name), "backup%llu", (unsigned long long)point);
        temp_path(backup, sizeof(backup), name);
        SX_CHECK(sx_dir_create_path(backup) == SX_OK);

        (void)fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) {
            sx_fault_plan stop = {0};
            stop.action = SX_FAULT_STOP;
            stop.trigger = point;
            stop.torn_write = true;
            stop.power_loss = true;
            stop.backup_dir = backup;
            stop.seed = point;
            stop.exit_status = 42;
            if (sx_fault_arm(&stop) != SX_OK) {
                _exit(1);
            }
            sx_data_dir child_dir;
            char child_message[SX_DATA_DIR_MESSAGE_SIZE];
            sx_data_dir_open(path, SX_DATA_DIR_INITIALIZE, &child_dir, child_message,
                             sizeof(child_message));
            _exit(2); /* the point was never reached */
        }
        int status = 0;
        SX_CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        SX_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);

        sx_err err =
            sx_data_dir_open(path, SX_DATA_DIR_INITIALIZE, &data_dir, message, sizeof(message));
        if (err != SX_OK) {
            fprintf(stderr, "    point %llu: %s\n", (unsigned long long)point, message);
        }
        SX_CHECK(err == SX_OK);
        sx_data_dir_close(&data_dir);
        SX_CHECK(sx_data_dir_open(path, SX_DATA_DIR_EXISTING, &data_dir, message,
                                  sizeof(message)) == SX_OK);
        sx_data_dir_close(&data_dir);
    }
}

static const sx_test_case data_dir_cases[] = {
    {"format_round_trip", test_format_round_trip},
    {"format_rejects_malformed_text", test_format_rejects_malformed_text},
    {"format_feature_checks", test_format_feature_checks},
    {"initializes_and_reopens", test_initializes_and_reopens},
    {"lock_is_held", test_lock_is_held},
    {"refuses_foreign_directory", test_refuses_foreign_directory},
    {"checks_permissions", test_checks_permissions},
    {"format_problems_refuse_opening", test_format_problems_refuse_opening},
    {"removes_temporary_leftovers", test_removes_temporary_leftovers},
    {"initialization_survives_crashes", test_initialization_survives_crashes},
};

const sx_test_suite sx_test_suite_data_dir = SX_TEST_SUITE("data_dir", data_dir_cases);
