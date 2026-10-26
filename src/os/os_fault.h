/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Fault injection for crash and error tests. See docs/DESIGN.md section 13.
 *
 * Every call of the file layer (os/os_file.h) that changes the file system is a fault
 * point: creating a file or directory, writing, syncing a file, truncating, preallocating,
 * syncing a directory, renaming, and removing. Until a test arms fault injection, a point
 * costs one relaxed atomic load. Armed, the layer counts the points a workload passes and
 * can act on one of them:
 *
 *   - fail it (or it and every later one) with a chosen errno, changing nothing;
 *   - stop the process there with _exit, optionally after writing only part of the data
 *     of a write (a torn write) and after a simulated power loss.
 *
 * A simulated power loss undoes what a real one may lose. While armed with a backup
 * directory, the layer tracks the changes made since the last sync: the old contents of
 * byte ranges that were overwritten or cut off, the size of each file at its last sync,
 * and the entries created, renamed, and removed in each directory since that directory's
 * last sync (removed and replaced files are kept in the backup directory until then).
 * sx_fault_power_loss then picks, from the seed:
 *
 *   - for each file with unsynced data, whether that data survived, was lost (old bytes
 *     restored, size reset), or was replaced with random bytes;
 *   - a point in the sequence of unsynced directory changes: changes before it survive,
 *     later ones are undone, like the ordered journal of a file system.
 *
 * Changes made before arming count as durable. These functions are internal test hooks,
 * not part of the public API, and assume that no file layer call runs while a test arms,
 * disarms, or simulates a power loss.
 */

#ifndef SX_OS_OS_FAULT_H
#define SX_OS_OS_FAULT_H

#include "common/error.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum sx_fault_kind {
    SX_FAULT_CREATE = 0, /* creating a file or a directory */
    SX_FAULT_WRITE,
    SX_FAULT_SYNC, /* syncing a file */
    SX_FAULT_TRUNCATE,
    SX_FAULT_PREALLOCATE,
    SX_FAULT_DIR_SYNC,
    SX_FAULT_RENAME,
    SX_FAULT_REMOVE,
    SX_FAULT_KIND_COUNT,
} sx_fault_kind;

typedef enum sx_fault_action {
    SX_FAULT_COUNT = 0, /* only count points */
    SX_FAULT_FAIL,      /* the triggered call fails with `error_number` and changes nothing */
    SX_FAULT_STOP,      /* the process stops at the triggered point with _exit */
} sx_fault_action;

typedef struct sx_fault_plan {
    sx_fault_action action;
    /* Number of the point to act on, counting from 1; 0 never acts. */
    uint64_t trigger;
    /* Count only points of `kind` (otherwise points of every kind count). */
    bool only_kind;
    sx_fault_kind kind;
    /* SX_FAULT_FAIL: the errno of the failure, and whether every later point fails too. */
    int error_number;
    bool sticky;
    /* SX_FAULT_STOP: write a random prefix of a triggered write first (a torn write). */
    bool torn_write;
    /* SX_FAULT_STOP: simulate a power loss before stopping (needs backup_dir). */
    bool power_loss;
    int exit_status;
    /*
     * Directory on the same file system as the files under test, for files that were
     * removed or replaced but may come back in a power loss. NULL disables tracking, and
     * with it power loss simulation.
     */
    const char* backup_dir;
    /* Seed for every random choice: torn write length and power loss outcomes. */
    uint64_t seed;
} sx_fault_plan;

/* Arms fault injection with `plan`, resetting the point count and the tracked changes. */
sx_err sx_fault_arm(const sx_fault_plan* plan);

/* Disarms fault injection and forgets the tracked changes. */
void sx_fault_disarm(void);

/* Number of points counted since arming. */
uint64_t sx_fault_point_count(void);

/*
 * Simulates a power loss now, then treats the resulting state as durable and keeps
 * tracking. Every file and directory handle must be closed afterwards, before the data is
 * opened again. SX_ERR_UNSUPPORTED if not armed with a backup directory.
 */
sx_err sx_fault_power_loss(void);

#endif /* SX_OS_OS_FAULT_H */
