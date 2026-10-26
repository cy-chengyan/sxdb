/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Fault injection and power loss simulation. See os/os_fault.h.
 *
 * This code only runs while a test has armed it; the file layer checks sx_fault_armed
 * first. Allocation failures while tracking abort the process: they can only happen in a
 * test that has armed the layer, and a silently incomplete undo log would make the test
 * meaningless.
 */

#include "os/os_fault.h"
#include "os/os_file_impl.h"

#include "common/compiler.h"
#include "common/rng.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

atomic_bool sx_fault_armed;

/* Old bytes of a range that an unsynced write or truncation overwrote or cut off. */
typedef struct saved_range {
    uint64_t offset;
    size_t length;
    uint8_t* bytes;
} saved_range;

/* A range written since the last sync. */
typedef struct written_range {
    uint64_t offset;
    size_t length;
} written_range;

/* Unsynced data changes of one file (one inode, whatever its names). */
typedef struct tracked_file {
    dev_t device;
    ino_t inode;
    int fd;               /* own descriptor, without O_DIRECT, so any name change is fine */
    uint64_t synced_size; /* size at the last sync, or when the file was first changed */
    bool changed;
    saved_range* saved;
    size_t saved_count;
    written_range* written;
    size_t written_count;
} tracked_file;

typedef enum change_type {
    CHANGE_CREATE,
    CHANGE_RENAME,
    CHANGE_REMOVE,
} change_type;

/* A change of a directory entry made since that directory's last sync. */
typedef struct entry_change {
    change_type type;
    int dir_fd; /* own descriptor of the directory */
    dev_t dir_device;
    ino_t dir_inode;
    bool is_dir;                         /* CHANGE_CREATE of a directory */
    char name[SX_FILE_NAME_MAX + 1];     /* created, removed, or old name */
    char new_name[SX_FILE_NAME_MAX + 1]; /* CHANGE_RENAME: new name */
    char backup[32]; /* removed or replaced file in the backup directory, or "" */
} entry_change;

static struct {
    pthread_mutex_t mutex;
    sx_fault_plan plan;
    uint64_t points;
    sx_rng rng;
    bool tracking;
    int backup_fd;
    uint64_t backup_counter;
    tracked_file* files;
    size_t file_count;
    entry_change* changes;
    size_t change_count;
} state = {.mutex = PTHREAD_MUTEX_INITIALIZER, .backup_fd = -1};

/* ---------------------------------------------------------------------------------------- */
/* Small helpers                                                                            */
/* ---------------------------------------------------------------------------------------- */

static void* grow_array(void* array, size_t count, size_t element_size) {
    /* Grows by one element; arrays here stay small, so no capacity bookkeeping. */
    void* grown = realloc(array, (count + 1) * element_size);
    if (grown == NULL) {
        fprintf(stderr, "fault injection: out of memory\n");
        abort();
    }
    return grown;
}

static bool read_all(int fd, uint8_t* buffer, size_t size, uint64_t offset) {
    while (size > 0) {
        ssize_t got = pread(fd, buffer, size, (off_t)offset);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return false;
        }
        buffer += got;
        size -= (size_t)got;
        offset += (uint64_t)got;
    }
    return true;
}

static bool write_all(int fd, const uint8_t* data, size_t size, uint64_t offset) {
    while (size > 0) {
        ssize_t written = pwrite(fd, data, size, (off_t)offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        data += written;
        size -= (size_t)written;
        offset += (uint64_t)written;
    }
    return true;
}

static void fill_random(uint8_t* buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)sx_rng_next(&state.rng);
    }
}

/*
 * Opens another descriptor for the file behind `fd`. On Linux it is a new open file
 * description without O_DIRECT, so the undo log can read and write unaligned ranges.
 */
static int open_own_descriptor(int fd) {
#if defined(__linux__)
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    int reopened = open(path, O_RDWR | O_CLOEXEC);
    if (reopened >= 0) {
        return reopened;
    }
#endif
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
}

/* ---------------------------------------------------------------------------------------- */
/* Fault points                                                                             */
/* ---------------------------------------------------------------------------------------- */

typedef enum point_outcome {
    POINT_PROCEED,
    POINT_FAIL,
    POINT_STOP,
} point_outcome;

/* Counts a point and returns what to do at it. Called with the mutex held. */
static point_outcome pass_point(sx_fault_kind kind) {
    const sx_fault_plan* plan = &state.plan;
    if (plan->only_kind && plan->kind != kind) {
        return POINT_PROCEED;
    }
    state.points++;
    if (plan->action == SX_FAULT_COUNT || plan->trigger == 0) {
        return POINT_PROCEED;
    }
    bool triggered = state.points == plan->trigger;
    if (plan->action == SX_FAULT_FAIL && plan->sticky && state.points > plan->trigger) {
        triggered = true;
    }
    if (!triggered) {
        return POINT_PROCEED;
    }
    return plan->action == SX_FAULT_FAIL ? POINT_FAIL : POINT_STOP;
}

static void simulate_power_loss(void);

/* Stops the process at a triggered point. Called with the mutex held; does not return. */
static void stop_process(void) {
    if (state.plan.power_loss && state.tracking) {
        simulate_power_loss();
    }
    _exit(state.plan.exit_status);
}

/*
 * Handles a point that needs nothing but the decision. Called with the mutex held;
 * returns SX_OK to proceed or the injected error.
 */
static sx_err check_point(sx_fault_kind kind, const char* operation, const char* name) {
    point_outcome outcome = pass_point(kind);
    if (outcome == POINT_FAIL) {
        return sx_os_fail(operation, name, state.plan.error_number);
    }
    if (outcome == POINT_STOP) {
        stop_process();
    }
    return SX_OK;
}

/* ---------------------------------------------------------------------------------------- */
/* Tracking data changes                                                                    */
/* ---------------------------------------------------------------------------------------- */

static tracked_file* find_file(dev_t device, ino_t inode) {
    for (size_t i = 0; i < state.file_count; i++) {
        if (state.files[i].device == device && state.files[i].inode == inode) {
            return &state.files[i];
        }
    }
    return NULL;
}

/* Returns the tracking entry of an open file, creating it on its first change. */
static tracked_file* track_file(int fd) {
    struct stat status;
    if (fstat(fd, &status) != 0) {
        return NULL;
    }
    tracked_file* found = find_file(status.st_dev, status.st_ino);
    if (found != NULL) {
        return found;
    }
    int own_fd = open_own_descriptor(fd);
    if (own_fd < 0) {
        return NULL;
    }
    state.files = grow_array(state.files, state.file_count, sizeof(tracked_file));
    tracked_file* added = &state.files[state.file_count++];
    memset(added, 0, sizeof(*added));
    added->device = status.st_dev;
    added->inode = status.st_ino;
    added->fd = own_fd;
    added->synced_size = (uint64_t)status.st_size;
    return added;
}

/* Saves the bytes of [offset, offset + length) that lie within the synced size. */
static void save_old_bytes(tracked_file* file, uint64_t offset, uint64_t length) {
    if (offset >= file->synced_size) {
        return;
    }
    uint64_t end = offset + length;
    if (end > file->synced_size) {
        end = file->synced_size;
    }
    size_t size = (size_t)(end - offset);
    uint8_t* bytes = malloc(size > 0 ? size : 1);
    if (bytes == NULL) {
        fprintf(stderr, "fault injection: out of memory\n");
        abort();
    }
    if (!read_all(file->fd, bytes, size, offset)) {
        /* The file is shorter than its synced size: it was cut since; nothing to save. */
        free(bytes);
        return;
    }
    file->saved = grow_array(file->saved, file->saved_count, sizeof(saved_range));
    file->saved[file->saved_count++] = (saved_range){offset, size, bytes};
}

static void forget_file_changes(tracked_file* file) {
    for (size_t i = 0; i < file->saved_count; i++) {
        free(file->saved[i].bytes);
    }
    free(file->saved);
    free(file->written);
    file->saved = NULL;
    file->saved_count = 0;
    file->written = NULL;
    file->written_count = 0;
    file->changed = false;
}

static void record_write(int fd, size_t size, uint64_t offset) {
    tracked_file* file = track_file(fd);
    if (file == NULL) {
        return;
    }
    save_old_bytes(file, offset, size);
    file->written = grow_array(file->written, file->written_count, sizeof(written_range));
    file->written[file->written_count++] = (written_range){offset, size};
    file->changed = true;
}

/* ---------------------------------------------------------------------------------------- */
/* Tracking directory entries                                                               */
/* ---------------------------------------------------------------------------------------- */

static entry_change* add_change(change_type type, sx_dir* dir, const char* name) {
    struct stat status;
    if (fstat(dir->fd, &status) != 0) {
        return NULL;
    }
    int own_fd = fcntl(dir->fd, F_DUPFD_CLOEXEC, 0);
    if (own_fd < 0) {
        return NULL;
    }
    state.changes = grow_array(state.changes, state.change_count, sizeof(entry_change));
    entry_change* change = &state.changes[state.change_count++];
    memset(change, 0, sizeof(*change));
    change->type = type;
    change->dir_fd = own_fd;
    change->dir_device = status.st_dev;
    change->dir_inode = status.st_ino;
    snprintf(change->name, sizeof(change->name), "%s", name);
    return change;
}

/* Picks a fresh name in the backup directory. */
static void next_backup_name(char* buffer, size_t capacity) {
    snprintf(buffer, capacity, "b%llu", (unsigned long long)state.backup_counter++);
}

/* The change is now durable: its backup, if any, is no longer needed. */
static void finalize_change(entry_change* change) {
    if (change->backup[0] != '\0') {
        unlinkat(state.backup_fd, change->backup, 0);
    }
    close(change->dir_fd);
}

static bool remove_tree(int parent_fd, const char* name);

static bool remove_tree_contents(int dir_fd) {
    int list_fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (list_fd < 0) {
        return false;
    }
    DIR* stream = fdopendir(list_fd);
    if (stream == NULL) {
        close(list_fd);
        return false;
    }
    bool ok = true;
    struct dirent* entry;
    while ((entry = readdir(stream)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        struct stat status;
        if (fstatat(dir_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
            ok = false;
            continue;
        }
        if (S_ISDIR(status.st_mode)) {
            ok = remove_tree(dir_fd, entry->d_name) && ok;
        } else if (unlinkat(dir_fd, entry->d_name, 0) != 0) {
            ok = false;
        }
    }
    closedir(stream);
    return ok;
}

/* Removes the directory `name` and everything in it. */
static bool remove_tree(int parent_fd, const char* name) {
    int dir_fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        return errno == ENOENT;
    }
    bool ok = remove_tree_contents(dir_fd);
    close(dir_fd);
    if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0 && errno != ENOENT) {
        ok = false;
    }
    return ok;
}

/* Undoes a change that the simulated power loss lost. */
static bool undo_change(const entry_change* change) {
    bool ok = true;
    switch (change->type) {
        case CHANGE_CREATE:
            if (change->is_dir) {
                ok = remove_tree(change->dir_fd, change->name);
            } else if (unlinkat(change->dir_fd, change->name, 0) != 0 && errno != ENOENT) {
                ok = false;
            }
            break;
        case CHANGE_RENAME:
            if (renameat(change->dir_fd, change->new_name, change->dir_fd, change->name) != 0) {
                ok = false;
            }
            if (change->backup[0] != '\0' &&
                renameat(state.backup_fd, change->backup, change->dir_fd, change->new_name) != 0) {
                ok = false;
            }
            break;
        case CHANGE_REMOVE:
            if (renameat(state.backup_fd, change->backup, change->dir_fd, change->name) != 0) {
                ok = false;
            }
            break;
    }
    close(change->dir_fd);
    return ok;
}

/* ---------------------------------------------------------------------------------------- */
/* Power loss                                                                               */
/* ---------------------------------------------------------------------------------------- */

/* Data written since the last sync was lost: restore the old bytes and the synced size. */
static bool drop_unsynced_data(tracked_file* file) {
    bool ok = ftruncate(file->fd, (off_t)file->synced_size) == 0;
    for (size_t i = file->saved_count; i > 0; i--) {
        const saved_range* range = &file->saved[i - 1];
        ok = write_all(file->fd, range->bytes, range->length, range->offset) && ok;
    }
    return ok;
}

/* Data written since the last sync reached the disk as garbage. */
static bool garble_unsynced_data(tracked_file* file) {
    struct stat status;
    if (fstat(file->fd, &status) != 0) {
        return false;
    }
    uint64_t size = (uint64_t)status.st_size;
    bool ok = true;
    for (size_t i = 0; i < file->written_count; i++) {
        const written_range* range = &file->written[i];
        if (range->offset >= size) {
            continue;
        }
        size_t length = range->length;
        if (range->offset + length > size) {
            length = (size_t)(size - range->offset);
        }
        uint8_t* noise = malloc(length > 0 ? length : 1);
        if (noise == NULL) {
            return false;
        }
        fill_random(noise, length);
        ok = write_all(file->fd, noise, length, range->offset) && ok;
        free(noise);
    }
    return ok;
}

static void forget_tracking(void) {
    for (size_t i = 0; i < state.file_count; i++) {
        forget_file_changes(&state.files[i]);
        close(state.files[i].fd);
    }
    free(state.files);
    state.files = NULL;
    state.file_count = 0;
    for (size_t i = 0; i < state.change_count; i++) {
        finalize_change(&state.changes[i]);
    }
    free(state.changes);
    state.changes = NULL;
    state.change_count = 0;
}

/* Applies a random power loss outcome. Called with the mutex held. */
static bool apply_power_loss(void) {
    bool ok = true;
    for (size_t i = 0; i < state.file_count; i++) {
        tracked_file* file = &state.files[i];
        if (!file->changed) {
            continue;
        }
        switch (sx_rng_below(&state.rng, 3)) {
            case 0: /* the data reached the disk */
                break;
            case 1:
                ok = drop_unsynced_data(file) && ok;
                break;
            default:
                ok = garble_unsynced_data(file) && ok;
                break;
        }
    }
    /* Directory changes survive up to a random point and are undone after it. */
    size_t cut = (size_t)sx_rng_below(&state.rng, state.change_count + 1);
    for (size_t i = state.change_count; i > cut; i--) {
        ok = undo_change(&state.changes[i - 1]) && ok;
    }
    for (size_t i = 0; i < cut; i++) {
        finalize_change(&state.changes[i]);
    }
    free(state.changes);
    state.changes = NULL;
    state.change_count = 0;
    return ok;
}

static void simulate_power_loss(void) {
    if (!apply_power_loss()) {
        fprintf(stderr, "fault injection: could not apply every power loss change\n");
    }
    /* Whatever is on disk now is the durable state. */
    forget_tracking();
}

/* ---------------------------------------------------------------------------------------- */
/* Hooks called by the file layer                                                           */
/* ---------------------------------------------------------------------------------------- */

sx_err sx_fault_on_create(sx_dir* dir, const char* name, const char* operation) {
    SX_UNUSED(dir);
    pthread_mutex_lock(&state.mutex);
    sx_err err = check_point(SX_FAULT_CREATE, operation, name);
    pthread_mutex_unlock(&state.mutex);
    return err;
}

void sx_fault_created(sx_dir* dir, const char* name, bool is_dir) {
    pthread_mutex_lock(&state.mutex);
    if (state.tracking) {
        entry_change* change = add_change(CHANGE_CREATE, dir, name);
        if (change != NULL) {
            change->is_dir = is_dir;
        }
    }
    pthread_mutex_unlock(&state.mutex);
}

sx_err sx_fault_on_write(sx_file* file, const void* data, size_t size, uint64_t offset) {
    pthread_mutex_lock(&state.mutex);
    point_outcome outcome = pass_point(SX_FAULT_WRITE);
    if (outcome == POINT_FAIL) {
        sx_err err = sx_os_fail("write file", file->name, state.plan.error_number);
        pthread_mutex_unlock(&state.mutex);
        return err;
    }
    if (state.tracking) {
        record_write(file->fd, size, offset);
    }
    if (outcome == POINT_STOP) {
        if (state.plan.torn_write && size > 0) {
            size_t prefix = (size_t)sx_rng_below(&state.rng, size);
            if (file->direct) {
                prefix &= ~(size_t)(SX_FILE_DIRECT_ALIGNMENT - 1);
            }
            write_all(file->fd, data, prefix, offset);
        }
        stop_process();
    }
    pthread_mutex_unlock(&state.mutex);
    return SX_OK;
}

sx_err sx_fault_on_sync(sx_file* file) {
    pthread_mutex_lock(&state.mutex);
    sx_err err = check_point(SX_FAULT_SYNC, "sync file", file->name);
    pthread_mutex_unlock(&state.mutex);
    return err;
}

void sx_fault_synced(sx_file* file) {
    pthread_mutex_lock(&state.mutex);
    struct stat status;
    if (state.tracking && fstat(file->fd, &status) == 0) {
        tracked_file* tracked = find_file(status.st_dev, status.st_ino);
        if (tracked != NULL) {
            forget_file_changes(tracked);
            tracked->synced_size = (uint64_t)status.st_size;
        }
    }
    pthread_mutex_unlock(&state.mutex);
}

sx_err sx_fault_on_truncate(sx_file* file, uint64_t size) {
    pthread_mutex_lock(&state.mutex);
    sx_err err = check_point(SX_FAULT_TRUNCATE, "truncate file", file->name);
    if (err == SX_OK && state.tracking) {
        tracked_file* tracked = track_file(file->fd);
        struct stat status;
        if (tracked != NULL && fstat(file->fd, &status) == 0) {
            uint64_t current = (uint64_t)status.st_size;
            if (size < current) {
                save_old_bytes(tracked, size, current - size);
            }
            tracked->changed = true;
        }
    }
    pthread_mutex_unlock(&state.mutex);
    return err;
}

sx_err sx_fault_on_preallocate(sx_file* file, uint64_t size) {
    SX_UNUSED(size);
    pthread_mutex_lock(&state.mutex);
    sx_err err = check_point(SX_FAULT_PREALLOCATE, "preallocate file", file->name);
    if (err == SX_OK && state.tracking) {
        tracked_file* tracked = track_file(file->fd);
        if (tracked != NULL) {
            tracked->changed = true;
        }
    }
    pthread_mutex_unlock(&state.mutex);
    return err;
}

sx_err sx_fault_on_dir_sync(sx_dir* dir) {
    pthread_mutex_lock(&state.mutex);
    sx_err err = check_point(SX_FAULT_DIR_SYNC, "sync directory", dir->path);
    pthread_mutex_unlock(&state.mutex);
    return err;
}

void sx_fault_dir_synced(sx_dir* dir) {
    pthread_mutex_lock(&state.mutex);
    struct stat status;
    if (state.tracking && fstat(dir->fd, &status) == 0) {
        /* Changes of this directory are durable now; keep the others in order. */
        size_t kept = 0;
        for (size_t i = 0; i < state.change_count; i++) {
            entry_change* change = &state.changes[i];
            if (change->dir_device == status.st_dev && change->dir_inode == status.st_ino) {
                finalize_change(change);
            } else {
                state.changes[kept++] = *change;
            }
        }
        state.change_count = kept;
    }
    pthread_mutex_unlock(&state.mutex);
}

sx_err sx_fault_rename(sx_dir* dir, const char* from, const char* to) {
    pthread_mutex_lock(&state.mutex);
    sx_err err = check_point(SX_FAULT_RENAME, "rename file", from);
    if (err != SX_OK) {
        pthread_mutex_unlock(&state.mutex);
        return err;
    }
    char backup[32] = "";
    struct stat status;
    if (state.tracking && fstatat(dir->fd, to, &status, AT_SYMLINK_NOFOLLOW) == 0) {
        /* Keep the file being replaced, in case the rename is lost. */
        next_backup_name(backup, sizeof(backup));
        if (linkat(dir->fd, to, state.backup_fd, backup, 0) != 0) {
            err = sx_os_fail("rename file", from, errno);
            pthread_mutex_unlock(&state.mutex);
            return err;
        }
    }
    if (renameat(dir->fd, from, dir->fd, to) != 0) {
        err = sx_os_fail("rename file", from, errno);
        if (backup[0] != '\0') {
            unlinkat(state.backup_fd, backup, 0);
        }
        pthread_mutex_unlock(&state.mutex);
        return err;
    }
    if (state.tracking) {
        entry_change* change = add_change(CHANGE_RENAME, dir, from);
        if (change != NULL) {
            snprintf(change->new_name, sizeof(change->new_name), "%s", to);
            snprintf(change->backup, sizeof(change->backup), "%s", backup);
        }
    }
    pthread_mutex_unlock(&state.mutex);
    return SX_OK;
}

sx_err sx_fault_remove(sx_dir* dir, const char* name) {
    pthread_mutex_lock(&state.mutex);
    sx_err err = check_point(SX_FAULT_REMOVE, "remove file", name);
    if (err != SX_OK) {
        pthread_mutex_unlock(&state.mutex);
        return err;
    }
    if (!state.tracking) {
        if (unlinkat(dir->fd, name, 0) != 0) {
            err = sx_os_fail("remove file", name, errno);
        }
        pthread_mutex_unlock(&state.mutex);
        return err;
    }
    /* Move the file aside instead of removing it, in case the removal is lost. */
    char backup[32];
    next_backup_name(backup, sizeof(backup));
    if (renameat(dir->fd, name, state.backup_fd, backup) != 0) {
        err = sx_os_fail("remove file", name, errno);
        pthread_mutex_unlock(&state.mutex);
        return err;
    }
    entry_change* change = add_change(CHANGE_REMOVE, dir, name);
    if (change != NULL) {
        snprintf(change->backup, sizeof(change->backup), "%s", backup);
    }
    pthread_mutex_unlock(&state.mutex);
    return SX_OK;
}

/* ---------------------------------------------------------------------------------------- */
/* Test interface                                                                           */
/* ---------------------------------------------------------------------------------------- */

static void disarm_locked(void) {
    atomic_store(&sx_fault_armed, false);
    forget_tracking();
    if (state.backup_fd >= 0) {
        close(state.backup_fd);
        state.backup_fd = -1;
    }
    state.tracking = false;
    state.points = 0;
}

sx_err sx_fault_arm(const sx_fault_plan* plan) {
    pthread_mutex_lock(&state.mutex);
    disarm_locked();
    state.plan = *plan;
    state.plan.backup_dir = NULL;
    sx_rng_seed(&state.rng, plan->seed);
    state.backup_counter = 0;
    if (plan->backup_dir != NULL) {
        state.backup_fd = open(plan->backup_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (state.backup_fd < 0) {
            sx_err err = sx_os_fail("open directory", plan->backup_dir, errno);
            pthread_mutex_unlock(&state.mutex);
            return err;
        }
        state.tracking = true;
    }
    atomic_store(&sx_fault_armed, true);
    pthread_mutex_unlock(&state.mutex);
    return SX_OK;
}

void sx_fault_disarm(void) {
    pthread_mutex_lock(&state.mutex);
    disarm_locked();
    pthread_mutex_unlock(&state.mutex);
}

uint64_t sx_fault_point_count(void) {
    pthread_mutex_lock(&state.mutex);
    uint64_t points = state.points;
    pthread_mutex_unlock(&state.mutex);
    return points;
}

sx_err sx_fault_power_loss(void) {
    pthread_mutex_lock(&state.mutex);
    if (!state.tracking) {
        pthread_mutex_unlock(&state.mutex);
        return SX_ERR_UNSUPPORTED;
    }
    bool ok = apply_power_loss();
    forget_tracking();
    pthread_mutex_unlock(&state.mutex);
    return ok ? SX_OK : SX_ERR_IO;
}
