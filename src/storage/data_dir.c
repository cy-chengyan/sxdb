/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "storage/data_dir.h"

#include "common/compiler.h"
#include "os/os.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOCK_NAME   "LOCK"
#define FORMAT_NAME "FORMAT"

/*
 * Entries an interrupted initialization can leave behind. Initialization writes FORMAT
 * last, so a directory holding only these is still empty for sxdb. Steps that make
 * initialization create more (the CATALOG, the WAL directory) add their names here.
 */
static const char* const initialization_leftovers[] = {
    LOCK_NAME,
    FORMAT_NAME ".tmp",
};

static sx_err fail(char* message, size_t capacity, sx_err err, const char* format, ...)
    SX_PRINTF_FORMAT(4, 5);

static sx_err fail(char* message, size_t capacity, sx_err err, const char* format, ...) {
    if (capacity > 0) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(message, capacity, format, args);
        va_end(args);
    }
    return err;
}

/* Fails with the message of the last file layer error. */
static sx_err fail_os(char* message, size_t capacity, sx_err err) {
    sx_os_last_error_message(message, capacity);
    return err;
}

static bool has_suffix(const char* name, const char* suffix) {
    size_t length = strlen(name);
    size_t suffix_length = strlen(suffix);
    return length >= suffix_length && strcmp(name + length - suffix_length, suffix) == 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Checks                                                                                   */
/* ---------------------------------------------------------------------------------------- */

static sx_err check_permissions(sx_dir* dir, const char* path, char* message, size_t capacity) {
    sx_dir_info info;
    sx_err err = sx_dir_get_info(dir, &info);
    if (err != SX_OK) {
        return fail_os(message, capacity, err);
    }
    uint32_t uid = sx_os_effective_uid();
    if (info.owner_uid != uid) {
        return fail(message, capacity, SX_ERR_PERMISSION,
                    "data directory \"%s\" is owned by uid %u, but sxdb runs as uid %u", path,
                    (unsigned)info.owner_uid, (unsigned)uid);
    }
    uint32_t mode = info.mode & 0777;
    if (mode != 0700 && mode != 0750) {
        return fail(message, capacity, SX_ERR_PERMISSION,
                    "data directory \"%s\" has permissions %04o; it must be 0700 or 0750", path,
                    (unsigned)mode);
    }
    return SX_OK;
}

/* What a directory holds, as far as deciding whether it is sxdb's. */
typedef struct contents {
    bool has_format;
    bool has_other;
    char other[SX_FILE_NAME_MAX + 1]; /* the first entry that is not sxdb's */
} contents;

static bool is_initialization_leftover(const char* name) {
    for (size_t i = 0; i < SX_ARRAY_SIZE(initialization_leftovers); i++) {
        if (strcmp(name, initialization_leftovers[i]) == 0) {
            return true;
        }
    }
    return false;
}

static sx_err note_entry(void* context, const char* name, bool is_dir) {
    SX_UNUSED(is_dir);
    contents* found = context;
    if (strcmp(name, FORMAT_NAME) == 0) {
        found->has_format = true;
    } else if (!is_initialization_leftover(name) && !found->has_other) {
        found->has_other = true;
        snprintf(found->other, sizeof(found->other), "%s", name);
    }
    return SX_OK;
}

static sx_err list_contents(sx_dir* dir, contents* found, char* message, size_t capacity) {
    memset(found, 0, sizeof(*found));
    sx_err err = sx_dir_list(dir, note_entry, found);
    if (err != SX_OK) {
        return fail_os(message, capacity, err);
    }
    return SX_OK;
}

/* Refuses a directory that is neither initialized nor empty. */
static sx_err refuse_foreign(const contents* found, const char* path, char* message,
                             size_t capacity) {
    return fail(message, capacity, SX_ERR_INVALID_ARG,
                "\"%s\" is not an sxdb data directory: it has no FORMAT file and is not "
                "empty (it contains \"%s\")",
                path, found->other);
}

/* ---------------------------------------------------------------------------------------- */
/* Lock                                                                                     */
/* ---------------------------------------------------------------------------------------- */

/* Reads the pid another process wrote into LOCK, for the message. */
static void read_lock_owner(sx_file* lock, char* owner, size_t capacity) {
    snprintf(owner, capacity, "unknown");
    uint64_t size = 0;
    char text[32];
    if (sx_file_get_size(lock, &size) != SX_OK || size == 0 || size >= sizeof(text)) {
        return;
    }
    if (sx_file_read_at(lock, text, (size_t)size, 0) != SX_OK) {
        return;
    }
    size_t length = (size_t)size;
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        length--;
    }
    text[length] = '\0';
    if (length > 0) {
        snprintf(owner, capacity, "%s", text);
    }
}

static sx_err take_lock(sx_dir* dir, const char* path, sx_file** lock, char* message,
                        size_t capacity) {
    sx_file* file = NULL;
    sx_err err = sx_file_open(dir, LOCK_NAME, SX_FILE_WRITE, &file);
    if (err == SX_ERR_NOT_FOUND) {
        err = sx_file_open(dir, LOCK_NAME, SX_FILE_CREATE, &file);
        if (err == SX_ERR_EXISTS) {
            err = sx_file_open(dir, LOCK_NAME, SX_FILE_WRITE, &file);
        }
    }
    if (err != SX_OK) {
        return fail_os(message, capacity, err);
    }
    err = sx_file_try_lock(file);
    if (err == SX_ERR_BUSY) {
        char owner[32];
        read_lock_owner(file, owner, sizeof(owner));
        sx_file_close(file);
        return fail(message, capacity, SX_ERR_BUSY, "data directory \"%s\" is in use by process %s",
                    path, owner);
    }
    if (err != SX_OK) {
        sx_file_close(file);
        return fail_os(message, capacity, err);
    }
    /*
     * Record the pid for the message another process shows. It is only a hint, so it is
     * not synced, and failing to write it (for example on a full disk) is not an error.
     */
    char pid[32];
    int length = snprintf(pid, sizeof(pid), "%llu\n", (unsigned long long)sx_os_process_id());
    if (sx_file_truncate(file, 0) == SX_OK && length > 0) {
        sx_file_write_at(file, pid, (size_t)length, 0);
    }
    *lock = file;
    return SX_OK;
}

/* ---------------------------------------------------------------------------------------- */
/* Opening                                                                                  */
/* ---------------------------------------------------------------------------------------- */

static sx_err open_or_create(const char* path, sx_data_dir_mode mode, sx_dir** dir, char* message,
                             size_t capacity) {
    sx_err err = sx_dir_open_path(path, dir);
    if (err == SX_ERR_NOT_FOUND && mode == SX_DATA_DIR_INITIALIZE) {
        err = sx_dir_create_path(path);
        if (err == SX_OK || err == SX_ERR_EXISTS) {
            err = sx_dir_open_path(path, dir);
        }
    }
    if (err == SX_ERR_NOT_FOUND) {
        return fail(message, capacity, err,
                    "data directory \"%s\" does not exist; run \"sxdb init\" to create it", path);
    }
    if (err != SX_OK) {
        return fail_os(message, capacity, err);
    }
    return SX_OK;
}

static sx_err check_format(sx_data_dir* data_dir, const char* path, char* message,
                           size_t capacity) {
    sx_err err = sx_format_read(data_dir->dir, &data_dir->format);
    if (err == SX_ERR_CORRUPT) {
        return fail(message, capacity, err, "the FORMAT file of data directory \"%s\" is damaged",
                    path);
    }
    if (err != SX_OK) {
        return fail_os(message, capacity, err);
    }
    const sx_format_feature* blocking = NULL;
    err = sx_format_check(&data_dir->format, true, &blocking);
    if (err != SX_OK) {
        return fail(message, capacity, err,
                    "data directory \"%s\" requires sxdb >= %s (it uses the feature \"%s\")", path,
                    blocking->version, blocking->name);
    }
    return SX_OK;
}

static sx_err initialize(sx_data_dir* data_dir, char* message, size_t capacity) {
    sx_err err = sx_format_init(&data_dir->format);
    if (err == SX_OK) {
        /* Written by atomic replacement, which also syncs the directory (and LOCK in it). */
        err = sx_format_write(data_dir->dir, &data_dir->format);
    }
    if (err != SX_OK) {
        return fail_os(message, capacity, err);
    }
    data_dir->initialized = true;
    return SX_OK;
}

typedef struct leftovers {
    sx_dir* dir;
    size_t removed;
    sx_err err;
} leftovers;

static sx_err remove_temporary(void* context, const char* name, bool is_dir) {
    leftovers* state = context;
    if (is_dir || !has_suffix(name, ".tmp")) {
        return SX_OK;
    }
    sx_err err = sx_dir_remove_file(state->dir, name);
    if (err != SX_OK && err != SX_ERR_NOT_FOUND) {
        state->err = err;
        return err;
    }
    state->removed++;
    return SX_OK;
}

static sx_err remove_leftovers(sx_dir* dir, char* message, size_t capacity) {
    leftovers state = {dir, 0, SX_OK};
    sx_err err = sx_dir_list(dir, remove_temporary, &state);
    if (err == SX_OK && state.removed > 0) {
        err = sx_dir_sync(dir);
    }
    if (err != SX_OK) {
        return fail_os(message, capacity, err);
    }
    return SX_OK;
}

static sx_err open_locked(sx_data_dir* data_dir, const char* path, sx_data_dir_mode mode,
                          char* message, size_t capacity) {
    sx_err err = check_permissions(data_dir->dir, path, message, capacity);
    if (err != SX_OK) {
        return err;
    }
    /* Decide before writing anything, so a foreign directory is never touched. */
    contents found;
    err = list_contents(data_dir->dir, &found, message, capacity);
    if (err != SX_OK) {
        return err;
    }
    if (!found.has_format && found.has_other) {
        return refuse_foreign(&found, path, message, capacity);
    }
    if (!found.has_format && mode == SX_DATA_DIR_EXISTING) {
        return fail(message, capacity, SX_ERR_NOT_FOUND,
                    "data directory \"%s\" is not initialized; run \"sxdb init\" first", path);
    }
    err = take_lock(data_dir->dir, path, &data_dir->lock, message, capacity);
    if (err != SX_OK) {
        return err;
    }
    /* Look again under the lock: another process may have initialized it meanwhile. */
    err = list_contents(data_dir->dir, &found, message, capacity);
    if (err != SX_OK) {
        return err;
    }
    if (found.has_format) {
        err = check_format(data_dir, path, message, capacity);
    } else if (found.has_other) {
        err = refuse_foreign(&found, path, message, capacity);
    } else if (mode == SX_DATA_DIR_INITIALIZE) {
        err = initialize(data_dir, message, capacity);
    } else {
        err = fail(message, capacity, SX_ERR_NOT_FOUND,
                   "data directory \"%s\" is not initialized; run \"sxdb init\" first", path);
    }
    if (err != SX_OK) {
        return err;
    }
    return remove_leftovers(data_dir->dir, message, capacity);
}

sx_err sx_data_dir_open(const char* path, sx_data_dir_mode mode, sx_data_dir* data_dir,
                        char* message, size_t message_capacity) {
    memset(data_dir, 0, sizeof(*data_dir));
    if (message_capacity > 0) {
        message[0] = '\0';
    }
    sx_err err = open_or_create(path, mode, &data_dir->dir, message, message_capacity);
    if (err == SX_OK) {
        err = open_locked(data_dir, path, mode, message, message_capacity);
    }
    if (err != SX_OK) {
        sx_data_dir_close(data_dir);
    }
    return err;
}

void sx_data_dir_close(sx_data_dir* data_dir) {
    sx_file_close(data_dir->lock);
    sx_dir_close(data_dir->dir);
    memset(data_dir, 0, sizeof(*data_dir));
}
