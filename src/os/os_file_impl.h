/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Internals shared by the file layer (os_file.c) and fault injection (os_fault.c).
 * Nothing outside src/os/ includes this header.
 */

#ifndef SX_OS_OS_FILE_IMPL_H
#define SX_OS_OS_FILE_IMPL_H

#include "os/os_file.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sx_dir {
    int fd;
    char* path;
};

struct sx_file {
    int fd;
    bool writable;
    bool direct;  /* the page cache is bypassed */
    bool aligned; /* opened with SX_FILE_DIRECT: transfers must be aligned */
    char name[SX_FILE_NAME_MAX + 1];
};

/* Maps errno to an error code. */
sx_err sx_os_error_from_errno(int error_number);

/*
 * Records a failed call for sx_os_last_error_message and returns the matching error code.
 * `operation` is a phrase such as "sync file"; `name` is the entry it applied to.
 */
sx_err sx_os_fail(const char* operation, const char* name, int error_number);

/*
 * Fault injection hooks (see os/os_fault.h). The file layer calls them only while
 * sx_fault_is_armed() is true. A hook named sx_fault_on_* runs before the operation and
 * returns SX_OK to let it proceed, or the error of an injected failure, already recorded
 * with sx_os_fail. Hooks that take over an operation (rename, remove) perform it
 * themselves, because tracking for a simulated power loss needs to keep the files they
 * would discard.
 */
extern atomic_bool sx_fault_armed;

static inline bool sx_fault_is_armed(void) {
    return atomic_load_explicit(&sx_fault_armed, memory_order_relaxed);
}

sx_err sx_fault_on_create(sx_dir* dir, const char* name, const char* operation);
void sx_fault_created(sx_dir* dir, const char* name, bool is_dir);
sx_err sx_fault_on_write(sx_file* file, const void* data, size_t size, uint64_t offset);
sx_err sx_fault_on_sync(sx_file* file);
void sx_fault_synced(sx_file* file);
sx_err sx_fault_on_truncate(sx_file* file, uint64_t size);
sx_err sx_fault_on_preallocate(sx_file* file, uint64_t size);
sx_err sx_fault_on_dir_sync(sx_dir* dir);
void sx_fault_dir_synced(sx_dir* dir);
sx_err sx_fault_rename(sx_dir* dir, const char* from, const char* to);
sx_err sx_fault_remove(sx_dir* dir, const char* name);

#endif /* SX_OS_OS_FILE_IMPL_H */
