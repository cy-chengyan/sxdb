/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * File layer. See docs/DESIGN.md section 6.3.
 *
 * Every file system call of the storage engine goes through these functions. Files are
 * opened, renamed, and removed relative to an open directory handle, and names are single
 * path components, so a path is resolved only once, when its directory is opened.
 *
 * Reads and writes are complete: short transfers and EINTR are retried, and reading past
 * the end of a file is reported as SX_ERR_CORRUPT. Every call that changes the file system
 * passes the fault injection hook (os/os_fault.h).
 *
 * When a call fails, the operation, the name it applied to, and errno are kept per thread;
 * sx_os_last_error_message formats them for an error message.
 *
 * A handle may be used by several threads at once for reads; writes, syncs, size changes,
 * and closing need the caller's own ordering.
 */

#ifndef SX_OS_OS_FILE_H
#define SX_OS_OS_FILE_H

#include "common/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sx_dir sx_dir;
typedef struct sx_file sx_file;

/* Flags for sx_file_open. */
enum {
    SX_FILE_WRITE = 1u << 0,  /* open for reading and writing; without it, read only */
    SX_FILE_CREATE = 1u << 1, /* create the file with mode 0600; SX_ERR_EXISTS if it exists */
    SX_FILE_DIRECT = 1u << 2, /* bypass the page cache: O_DIRECT on Linux, F_NOCACHE on macOS */
};

/*
 * Buffers, offsets, and lengths of reads and writes on a file opened with SX_FILE_DIRECT
 * must be multiples of this. It is checked on every platform, including where the page
 * cache is bypassed without alignment rules, so a mistake shows up everywhere.
 */
#define SX_FILE_DIRECT_ALIGNMENT 4096u

/* Longest name of a file or directory entry, in bytes. */
#define SX_FILE_NAME_MAX 255u

/* Ownership and permission bits of a directory, from sx_dir_get_info. */
typedef struct sx_dir_info {
    uint32_t owner_uid;
    uint32_t mode; /* permission bits only, for example 0700 */
} sx_dir_info;

/* Called once per entry by sx_dir_list. A result other than SX_OK stops the listing. */
typedef sx_err (*sx_dir_entry_fn)(void* context, const char* name, bool is_dir);

/* Opens the directory at `path`. */
sx_err sx_dir_open_path(const char* path, sx_dir** dir);

/* Opens the subdirectory `name` of `parent`. */
sx_err sx_dir_open(sx_dir* parent, const char* name, sx_dir** dir);

/* Closes a directory handle. NULL is ignored. */
void sx_dir_close(sx_dir* dir);

/* The path the directory was reached by, for messages. */
const char* sx_dir_path(const sx_dir* dir);

/* Creates the directory at `path` with mode 0700. Its parent must exist. */
sx_err sx_dir_create_path(const char* path);

/* Creates the subdirectory `name` of `parent` with mode 0700. */
sx_err sx_dir_create(sx_dir* parent, const char* name);

/* Makes the creations, renames, and removals of entries in `dir` durable. */
sx_err sx_dir_sync(sx_dir* dir);

/* Renames `from` to `to` within `dir`, atomically replacing an existing `to`. */
sx_err sx_dir_rename(sx_dir* dir, const char* from, const char* to);

/* Removes the file `name` from `dir`. */
sx_err sx_dir_remove_file(sx_dir* dir, const char* name);

/* Calls `fn` for every entry of `dir` except "." and "..", in no particular order. */
sx_err sx_dir_list(sx_dir* dir, sx_dir_entry_fn fn, void* context);

/* Reads the owner and permission bits of `dir`. */
sx_err sx_dir_get_info(sx_dir* dir, sx_dir_info* info);

/*
 * Replaces the file `name` in `dir` with `size` bytes of `data` so that, after a crash,
 * the file holds either its old or its new content: the data is written to
 * "<name>.tmp", synced, renamed over `name`, and the directory is synced.
 */
sx_err sx_dir_replace_file(sx_dir* dir, const char* name, const void* data, size_t size);

/*
 * Reads the whole file `name` of `dir` into `buffer`. SX_ERR_LIMIT if the file is larger
 * than `capacity`.
 */
sx_err sx_dir_read_file(sx_dir* dir, const char* name, void* buffer, size_t capacity, size_t* size);

/* Opens or creates the file `name` in `dir`; `flags` combines the SX_FILE_* flags. */
sx_err sx_file_open(sx_dir* dir, const char* name, unsigned flags, sx_file** file);

/* Closes a file handle. NULL is ignored. Closing releases a lock taken on the file. */
void sx_file_close(sx_file* file);

/* The name the file was opened with. */
const char* sx_file_name(const sx_file* file);

/*
 * True if the file bypasses the page cache: opened with SX_FILE_DIRECT on a file system
 * that supports it.
 */
bool sx_file_is_direct(const sx_file* file);

/* Reads exactly `size` bytes at `offset`. */
sx_err sx_file_read_at(sx_file* file, void* buffer, size_t size, uint64_t offset);

/* Writes exactly `size` bytes at `offset`. */
sx_err sx_file_write_at(sx_file* file, const void* data, size_t size, uint64_t offset);

/*
 * Makes the file's data durable. After a failure the data written since the last
 * successful sync must be treated as lost, and the sync is not to be retried (see
 * docs/DESIGN.md section 6.3).
 */
sx_err sx_file_sync(sx_file* file);

/* Reads the size of the file. */
sx_err sx_file_get_size(sx_file* file, uint64_t* size);

/* Sets the size of the file, cutting off or zero-extending its end. */
sx_err sx_file_truncate(sx_file* file, uint64_t size);

/*
 * Allocates disk blocks for the first `size` bytes and makes the file at least that long;
 * new bytes read as zeros. A file already that long is left as it is.
 */
sx_err sx_file_preallocate(sx_file* file, uint64_t size);

/*
 * Takes an exclusive advisory lock on the file (flock), without waiting. SX_ERR_BUSY if
 * another open file holds it. The lock is released when the file is closed or the
 * process exits.
 */
sx_err sx_file_try_lock(sx_file* file);

/* errno of the last failed call of the file layer on this thread, or 0. */
int sx_os_last_errno(void);

/*
 * Formats the last failure of the file layer on this thread, for example
 * `could not sync file "000017.sxs": Input/output error`.
 */
void sx_os_last_error_message(char* buffer, size_t capacity);

#endif /* SX_OS_OS_FILE_H */
