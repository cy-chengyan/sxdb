/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sx_test.h"

#include "os/os_fault.h"
#include "os/os_file.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Seeds tried for every power loss scenario; enough to see every outcome. */
#define SEED_COUNT 40

typedef struct fixture {
    char data_path[512];
    char backup_path[512];
    sx_dir* data;
} fixture;

/* Creates "<temp>/data" and "<temp>/backup", then opens the data directory. */
static bool fixture_open(fixture* fx, const char* name) {
    snprintf(fx->data_path, sizeof(fx->data_path), "%s/%s", sx_test_temp_dir(), name);
    snprintf(fx->backup_path, sizeof(fx->backup_path), "%s/%s-backup", sx_test_temp_dir(), name);
    fx->data = NULL;
    return sx_dir_create_path(fx->data_path) == SX_OK &&
           sx_dir_create_path(fx->backup_path) == SX_OK &&
           sx_dir_open_path(fx->data_path, &fx->data) == SX_OK;
}

static sx_err arm_tracking(const fixture* fx, uint64_t seed) {
    sx_fault_plan plan = {0};
    plan.action = SX_FAULT_COUNT;
    plan.backup_dir = fx->backup_path;
    plan.seed = seed;
    return sx_fault_arm(&plan);
}

/* Writes a whole small file and syncs it and its directory. */
static sx_err write_durable_file(sx_dir* dir, const char* name, const char* content) {
    sx_file* file = NULL;
    sx_err err = sx_file_open(dir, name, SX_FILE_CREATE, &file);
    if (err != SX_OK) {
        return err;
    }
    err = sx_file_write_at(file, content, strlen(content), 0);
    if (err == SX_OK) {
        err = sx_file_sync(file);
    }
    sx_file_close(file);
    if (err == SX_OK) {
        err = sx_dir_sync(dir);
    }
    return err;
}

/* Reads a small file as a string; returns false if it does not exist. */
static bool read_text(sx_dir* dir, const char* name, char* buffer, size_t capacity) {
    size_t size = 0;
    if (sx_dir_read_file(dir, name, buffer, capacity - 1, &size) != SX_OK) {
        return false;
    }
    buffer[size] = '\0';
    return true;
}

static void test_counts_points(void) {
    fixture fx;
    SX_CHECK(fixture_open(&fx, "count"));
    sx_fault_plan plan = {0};
    SX_CHECK(sx_fault_arm(&plan) == SX_OK);

    sx_file* file = NULL;
    SX_CHECK(sx_file_open(fx.data, "f", SX_FILE_CREATE, &file) == SX_OK); /* create */
    SX_CHECK(sx_file_write_at(file, "x", 1, 0) == SX_OK);                 /* write */
    SX_CHECK(sx_file_sync(file) == SX_OK);                                /* sync */
    SX_CHECK(sx_file_truncate(file, 0) == SX_OK);                         /* truncate */
    SX_CHECK(sx_file_preallocate(file, 4096) == SX_OK);                   /* preallocate */
    sx_file_close(file);
    SX_CHECK(sx_dir_rename(fx.data, "f", "g") == SX_OK); /* rename */
    SX_CHECK(sx_dir_remove_file(fx.data, "g") == SX_OK); /* remove */
    SX_CHECK(sx_dir_sync(fx.data) == SX_OK);             /* directory sync */
    SX_CHECK_EQ_U64(sx_fault_point_count(), 8);

    /* Reads are not fault points. */
    char buffer[4];
    size_t size = 0;
    SX_CHECK(sx_dir_read_file(fx.data, "missing", buffer, sizeof(buffer), &size) ==
             SX_ERR_NOT_FOUND);
    SX_CHECK_EQ_U64(sx_fault_point_count(), 8);
    sx_fault_disarm();
    sx_dir_close(fx.data);
}

static void test_counts_one_kind(void) {
    fixture fx;
    SX_CHECK(fixture_open(&fx, "kind"));
    sx_fault_plan plan = {0};
    plan.only_kind = true;
    plan.kind = SX_FAULT_SYNC;
    SX_CHECK(sx_fault_arm(&plan) == SX_OK);
    SX_CHECK(write_durable_file(fx.data, "a", "1") == SX_OK);
    SX_CHECK(write_durable_file(fx.data, "b", "2") == SX_OK);
    SX_CHECK_EQ_U64(sx_fault_point_count(), 2);
    sx_fault_disarm();
    sx_dir_close(fx.data);
}

static void test_fails_chosen_point(void) {
    fixture fx;
    SX_CHECK(fixture_open(&fx, "fail"));
    sx_fault_plan plan = {0};
    plan.action = SX_FAULT_FAIL;
    plan.trigger = 2;
    plan.error_number = EIO;
    SX_CHECK(sx_fault_arm(&plan) == SX_OK);

    sx_file* file = NULL;
    SX_CHECK(sx_file_open(fx.data, "f", SX_FILE_CREATE, &file) == SX_OK);
    SX_CHECK(sx_file_write_at(file, "data", 4, 0) == SX_ERR_IO);
    SX_CHECK_EQ_U64(sx_os_last_errno(), EIO);
    char message[256];
    sx_os_last_error_message(message, sizeof(message));
    SX_CHECK(strncmp(message, "could not write file \"f\": ", 26) == 0);

    /* The failed write changed nothing; the next one is not triggered. */
    uint64_t size = 1;
    SX_CHECK(sx_file_get_size(file, &size) == SX_OK);
    SX_CHECK_EQ_U64(size, 0);
    SX_CHECK(sx_file_write_at(file, "data", 4, 0) == SX_OK);
    sx_file_close(file);
    sx_fault_disarm();
    sx_dir_close(fx.data);
}

static void test_sticky_no_space(void) {
    fixture fx;
    SX_CHECK(fixture_open(&fx, "sticky"));
    sx_fault_plan plan = {0};
    plan.action = SX_FAULT_FAIL;
    plan.only_kind = true;
    plan.kind = SX_FAULT_WRITE;
    plan.trigger = 1;
    plan.sticky = true;
    plan.error_number = ENOSPC;
    SX_CHECK(sx_fault_arm(&plan) == SX_OK);
    sx_file* file = NULL;
    SX_CHECK(sx_file_open(fx.data, "f", SX_FILE_CREATE, &file) == SX_OK);
    for (int i = 0; i < 3; i++) {
        SX_CHECK(sx_file_write_at(file, "x", 1, 0) == SX_ERR_NO_SPACE);
    }
    SX_CHECK(sx_file_sync(file) == SX_OK);
    sx_file_close(file);
    sx_fault_disarm();
    sx_dir_close(fx.data);
}

/* Unsynced data survives, is lost, or turns into garbage; synced data always survives. */
static void test_power_loss_unsynced_data(void) {
    bool seen_kept = false;
    bool seen_lost = false;
    bool seen_garbled = false;
    for (uint64_t seed = 1; seed <= SEED_COUNT; seed++) {
        fixture fx;
        char name[32];
        snprintf(name, sizeof(name), "data%llu", (unsigned long long)seed);
        SX_CHECK(fixture_open(&fx, name));
        SX_CHECK(write_durable_file(fx.data, "log", "synced:") == SX_OK);
        SX_CHECK(arm_tracking(&fx, seed) == SX_OK);

        sx_file* file = NULL;
        SX_CHECK(sx_file_open(fx.data, "log", SX_FILE_WRITE, &file) == SX_OK);
        SX_CHECK(sx_file_write_at(file, "SYNCED", 6, 0) == SX_OK);   /* overwrite */
        SX_CHECK(sx_file_write_at(file, "unsynced", 8, 7) == SX_OK); /* append */
        sx_file_close(file);
        SX_CHECK(sx_fault_power_loss() == SX_OK);

        char text[32];
        SX_CHECK(read_text(fx.data, "log", text, sizeof(text)));
        if (strcmp(text, "SYNCED:unsynced") == 0) {
            seen_kept = true;
        } else if (strcmp(text, "synced:") == 0) {
            seen_lost = true;
        } else {
            SX_CHECK_EQ_U64(strlen(text) <= 15, 1);
            seen_garbled = true;
        }

        /* After a sync, a power loss changes nothing. */
        SX_CHECK(sx_file_open(fx.data, "log", SX_FILE_WRITE, &file) == SX_OK);
        SX_CHECK(sx_file_truncate(file, 0) == SX_OK);
        SX_CHECK(sx_file_write_at(file, "final", 5, 0) == SX_OK);
        SX_CHECK(sx_file_sync(file) == SX_OK);
        sx_file_close(file);
        SX_CHECK(sx_fault_power_loss() == SX_OK);
        SX_CHECK(read_text(fx.data, "log", text, sizeof(text)));
        SX_CHECK_EQ_STR(text, "final");
        sx_fault_disarm();
        sx_dir_close(fx.data);
    }
    SX_CHECK(seen_kept && seen_lost && seen_garbled);
}

/* A cut-off tail comes back when the truncation is lost. */
static void test_power_loss_truncation(void) {
    bool seen_restored = false;
    for (uint64_t seed = 1; seed <= SEED_COUNT; seed++) {
        fixture fx;
        char name[32];
        snprintf(name, sizeof(name), "trunc%llu", (unsigned long long)seed);
        SX_CHECK(fixture_open(&fx, name));
        SX_CHECK(write_durable_file(fx.data, "wal", "record1record2") == SX_OK);
        SX_CHECK(arm_tracking(&fx, seed) == SX_OK);
        sx_file* file = NULL;
        SX_CHECK(sx_file_open(fx.data, "wal", SX_FILE_WRITE, &file) == SX_OK);
        SX_CHECK(sx_file_truncate(file, 7) == SX_OK);
        sx_file_close(file);
        SX_CHECK(sx_fault_power_loss() == SX_OK);
        char text[32];
        SX_CHECK(read_text(fx.data, "wal", text, sizeof(text)));
        if (strcmp(text, "record1record2") == 0) {
            seen_restored = true;
        } else {
            SX_CHECK_EQ_STR(text, "record1");
        }
        sx_fault_disarm();
        sx_dir_close(fx.data);
    }
    SX_CHECK(seen_restored);
}

/*
 * Creating "new" and renaming it over "old" without a directory sync: the changes survive
 * in order up to a random point.
 */
static void test_power_loss_directory_changes(void) {
    bool seen[3] = {false, false, false};
    for (uint64_t seed = 1; seed <= SEED_COUNT; seed++) {
        fixture fx;
        char name[32];
        snprintf(name, sizeof(name), "dir%llu", (unsigned long long)seed);
        SX_CHECK(fixture_open(&fx, name));
        SX_CHECK(write_durable_file(fx.data, "old", "A") == SX_OK);
        SX_CHECK(arm_tracking(&fx, seed) == SX_OK);

        sx_file* file = NULL;
        SX_CHECK(sx_file_open(fx.data, "new", SX_FILE_CREATE, &file) == SX_OK);
        SX_CHECK(sx_file_write_at(file, "B", 1, 0) == SX_OK);
        SX_CHECK(sx_file_sync(file) == SX_OK);
        sx_file_close(file);
        SX_CHECK(sx_dir_rename(fx.data, "new", "old") == SX_OK);
        SX_CHECK(sx_fault_power_loss() == SX_OK);

        char old_text[8];
        char new_text[8];
        bool has_old = read_text(fx.data, "old", old_text, sizeof(old_text));
        bool has_new = read_text(fx.data, "new", new_text, sizeof(new_text));
        SX_CHECK(has_old);
        if (strcmp(old_text, "B") == 0) {
            SX_CHECK(!has_new); /* both changes survived */
            seen[2] = true;
        } else if (has_new) {
            SX_CHECK_EQ_STR(old_text, "A"); /* the creation survived, the rename did not */
            SX_CHECK_EQ_STR(new_text, "B");
            seen[1] = true;
        } else {
            SX_CHECK_EQ_STR(old_text, "A"); /* neither survived */
            seen[0] = true;
        }
        sx_fault_disarm();
        sx_dir_close(fx.data);
    }
    SX_CHECK(seen[0] && seen[1] && seen[2]);
}

/* Removals and directory creations are undone too; a directory sync makes them stick. */
static void test_power_loss_remove_and_mkdir(void) {
    bool seen_back = false;
    for (uint64_t seed = 1; seed <= SEED_COUNT; seed++) {
        fixture fx;
        char name[32];
        snprintf(name, sizeof(name), "rm%llu", (unsigned long long)seed);
        SX_CHECK(fixture_open(&fx, name));
        SX_CHECK(write_durable_file(fx.data, "segment", "S") == SX_OK);
        SX_CHECK(arm_tracking(&fx, seed) == SX_OK);
        SX_CHECK(sx_dir_create(fx.data, "p1") == SX_OK);
        sx_dir* partition = NULL;
        SX_CHECK(sx_dir_open(fx.data, "p1", &partition) == SX_OK);
        SX_CHECK(write_durable_file(partition, "inner", "I") == SX_OK);
        sx_dir_close(partition);
        SX_CHECK(sx_dir_remove_file(fx.data, "segment") == SX_OK);
        SX_CHECK(sx_fault_power_loss() == SX_OK);

        char text[8];
        bool has_segment = read_text(fx.data, "segment", text, sizeof(text));
        sx_dir* reopened = NULL;
        bool has_partition = sx_dir_open(fx.data, "p1", &reopened) == SX_OK;
        sx_dir_close(reopened);
        if (has_segment) {
            SX_CHECK_EQ_STR(text, "S");
            seen_back = true;
        } else {
            SX_CHECK(has_partition); /* the earlier change survived */
        }

        /* Synced removal: gone for good. */
        if (has_segment) {
            SX_CHECK(sx_dir_remove_file(fx.data, "segment") == SX_OK);
            SX_CHECK(sx_dir_sync(fx.data) == SX_OK);
            SX_CHECK(sx_fault_power_loss() == SX_OK);
            SX_CHECK(!read_text(fx.data, "segment", text, sizeof(text)));
        }
        sx_fault_disarm();
        sx_dir_close(fx.data);
    }
    SX_CHECK(seen_back);
}

/* Runs `steps` in a child process armed with `plan`; returns its exit status or -1. */
static int run_child(const sx_fault_plan* plan, sx_dir* dir) {
    (void)fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        if (sx_fault_arm(plan) != SX_OK) {
            _exit(90);
        }
        sx_file* file = NULL;
        if (sx_file_open(dir, "f", SX_FILE_CREATE, &file) != SX_OK) {
            _exit(91);
        }
        if (sx_file_write_at(file, "0123456789", 10, 0) != SX_OK) {
            _exit(92);
        }
        _exit(0);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

static void test_stops_process(void) {
    fixture fx;
    SX_CHECK(fixture_open(&fx, "stop"));
    sx_fault_plan plan = {0};
    plan.action = SX_FAULT_STOP;
    plan.trigger = 2;
    plan.exit_status = 42;
    SX_CHECK_EQ_I64(run_child(&plan, fx.data), 42);
    uint64_t size = 1;
    sx_file* file = NULL;
    SX_CHECK(sx_file_open(fx.data, "f", 0, &file) == SX_OK);
    SX_CHECK(sx_file_get_size(file, &size) == SX_OK);
    SX_CHECK_EQ_U64(size, 0); /* stopped before the write */
    sx_file_close(file);

    /* A torn write leaves a strict prefix. */
    SX_CHECK(sx_dir_remove_file(fx.data, "f") == SX_OK);
    plan.torn_write = true;
    plan.seed = 7;
    SX_CHECK_EQ_I64(run_child(&plan, fx.data), 42);
    SX_CHECK(sx_file_open(fx.data, "f", 0, &file) == SX_OK);
    SX_CHECK(sx_file_get_size(file, &size) == SX_OK);
    SX_CHECK(size < 10);
    char prefix[10];
    SX_CHECK(sx_file_read_at(file, prefix, (size_t)size, 0) == SX_OK);
    SX_CHECK_EQ_MEM(prefix, "0123456789", (size_t)size);
    sx_file_close(file);
    sx_dir_close(fx.data);
}

static const sx_test_case os_fault_cases[] = {
    {"counts_points", test_counts_points},
    {"counts_one_kind", test_counts_one_kind},
    {"fails_chosen_point", test_fails_chosen_point},
    {"sticky_no_space", test_sticky_no_space},
    {"power_loss_unsynced_data", test_power_loss_unsynced_data},
    {"power_loss_truncation", test_power_loss_truncation},
    {"power_loss_directory_changes", test_power_loss_directory_changes},
    {"power_loss_remove_and_mkdir", test_power_loss_remove_and_mkdir},
    {"stops_process", test_stops_process},
};

const sx_test_suite sx_test_suite_os_fault = SX_TEST_SUITE("os_fault", os_fault_cases);
