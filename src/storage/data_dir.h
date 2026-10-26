/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Opening a data directory. See docs/DESIGN.md section 4.1.
 *
 * In order: the directory must be owned by the user sxdb runs as and have mode 0700 or
 * 0750; a directory that is not empty and has no FORMAT file is refused without writing
 * anything into it; the LOCK file is locked; a missing or empty directory is initialized
 * if the caller asks for it (FORMAT is written last); the FORMAT feature flags are
 * checked; and files left by interrupted atomic replacements ("*.tmp") are removed.
 *
 * The directory stays locked until sx_data_dir_close.
 */

#ifndef SX_STORAGE_DATA_DIR_H
#define SX_STORAGE_DATA_DIR_H

#include "common/error.h"
#include "os/os_file.h"
#include "storage/format.h"

#include <stdbool.h>
#include <stddef.h>

/* What to do with a directory that has not been initialized. */
typedef enum sx_data_dir_mode {
    /* Refuse it: the server, which needs "sxdb init" to set a password first. */
    SX_DATA_DIR_EXISTING,
    /* Initialize a missing or empty directory: the embedded library and "sxdb init". */
    SX_DATA_DIR_INITIALIZE,
} sx_data_dir_mode;

typedef struct sx_data_dir {
    sx_dir* dir;
    sx_file* lock;
    sx_format format;
    bool initialized; /* this call initialized the directory */
} sx_data_dir;

/* Room for the explanation sx_data_dir_open gives on failure. */
#define SX_DATA_DIR_MESSAGE_SIZE 512u

/*
 * Opens the data directory at `path`. On failure, `message` explains why in a sentence
 * for the user, and the error code says what kind of failure it was:
 *   SX_ERR_NOT_FOUND     the directory is missing or not initialized (in EXISTING mode)
 *   SX_ERR_PERMISSION    wrong owner or permissions
 *   SX_ERR_INVALID_ARG   not an sxdb data directory
 *   SX_ERR_BUSY          another process has it open
 *   SX_ERR_CORRUPT       the FORMAT file is damaged
 *   SX_ERR_UNSUPPORTED   it needs a newer sxdb
 * or an IO error.
 */
sx_err sx_data_dir_open(const char* path, sx_data_dir_mode mode, sx_data_dir* data_dir,
                        char* message, size_t message_capacity);

/* Unlocks and closes the directory. */
void sx_data_dir_close(sx_data_dir* data_dir);

#endif /* SX_STORAGE_DATA_DIR_H */
