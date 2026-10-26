/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "os/os.h"
#include "os/os_file_impl.h"

#include "common/compiler.h"
#include "common/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------------------- */
/* Helpers used before the file layer existed (logging writes through these).               */
/* ---------------------------------------------------------------------------------------- */

sx_err sx_os_write_all(int fd, const void* data, size_t size) {
    const char* bytes = data;
    while (size > 0) {
        ssize_t written = write(fd, bytes, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SX_ERR_IO;
        }
        bytes += written;
        size -= (size_t)written;
    }
    return SX_OK;
}

sx_err sx_os_fsync_dir(const char* dir_path) {
    int fd = open(dir_path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return SX_ERR_IO;
    }
    int rc = fsync(fd);
    close(fd);
    if (rc != 0) {
        return SX_ERR_IO;
    }
    return SX_OK;
}

/* ---------------------------------------------------------------------------------------- */
/* Errors                                                                                   */
/* ---------------------------------------------------------------------------------------- */

/* Paths of directories opened by path can be longer than an entry name. */
#define ERROR_NAME_SIZE 1024

/* Longest path sx_dir_create_path accepts. */
#define PATH_SIZE 4096

/* The last failure on this thread. error_number 0 means an unexpected end of file. */
typedef struct last_error {
    const char* operation;
    char name[ERROR_NAME_SIZE];
    int error_number;
} last_error;

static _Thread_local last_error thread_error;

sx_err sx_os_error_from_errno(int error_number) {
    switch (error_number) {
        case ENOENT:
        case ENOTDIR:
            return SX_ERR_NOT_FOUND;
        case EEXIST:
        case ENOTEMPTY:
            return SX_ERR_EXISTS;
        case EACCES:
        case EPERM:
            return SX_ERR_PERMISSION;
        case ENOSPC:
        case EDQUOT:
            return SX_ERR_NO_SPACE;
        case EWOULDBLOCK:
            return SX_ERR_BUSY;
        case ENOMEM:
            return SX_ERR_NOMEM;
        case EINVAL:
        case ENAMETOOLONG:
            return SX_ERR_INVALID_ARG;
        default:
            return SX_ERR_IO;
    }
}

static void remember_failure(const char* operation, const char* name, int error_number) {
    thread_error.operation = operation;
    snprintf(thread_error.name, sizeof(thread_error.name), "%s", name != NULL ? name : "");
    thread_error.error_number = error_number;
}

sx_err sx_os_fail(const char* operation, const char* name, int error_number) {
    remember_failure(operation, name, error_number);
    sx_err err = sx_os_error_from_errno(error_number);
    return err != SX_OK ? err : SX_ERR_IO;
}

/* A read reached the end of the file before it had all the bytes it asked for. */
static sx_err fail_end_of_file(const char* name) {
    remember_failure("read file", name, 0);
    return SX_ERR_CORRUPT;
}

int sx_os_last_errno(void) {
    return thread_error.error_number;
}

/* Writes the text of `error_number` into `buffer`, with either strerror_r variant. */
static void describe_errno(int error_number, char* buffer, size_t capacity) {
    if (error_number == 0) {
        snprintf(buffer, capacity, "unexpected end of file");
        return;
    }
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    /* The GNU variant may return a static string instead of filling the buffer. */
    const char* text = strerror_r(error_number, buffer, capacity);
    if (text != buffer) {
        snprintf(buffer, capacity, "%s", text);
    }
#else
    if (strerror_r(error_number, buffer, capacity) != 0) {
        snprintf(buffer, capacity, "error %d", error_number);
    }
#endif
}

void sx_os_last_error_message(char* buffer, size_t capacity) {
    if (capacity == 0) {
        return;
    }
    if (thread_error.operation == NULL) {
        snprintf(buffer, capacity, "no file system error");
        return;
    }
    char reason[128];
    describe_errno(thread_error.error_number, reason, sizeof(reason));
    snprintf(buffer, capacity, "could not %s \"%s\": %s", thread_error.operation, thread_error.name,
             reason);
}

/* ---------------------------------------------------------------------------------------- */
/* Platform primitives                                                                      */
/* ---------------------------------------------------------------------------------------- */

/* A name of a single path component: not empty, no '/', not "." or "..". */
static bool is_valid_name(const char* name) {
    size_t length = strnlen(name, SX_FILE_NAME_MAX + 1);
    if (length == 0 || length > SX_FILE_NAME_MAX) {
        return false;
    }
    if (strchr(name, '/') != NULL) {
        return false;
    }
    return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

#if defined(__APPLE__)
/*
 * fsync on macOS only reaches the drive's volatile cache; F_FULLFSYNC asks the drive to
 * write it to stable storage. File systems that do not support F_FULLFSYNC (some network
 * file systems) fall back to fsync. Only those errors fall back: after an IO error the
 * sync must fail, not be retried in another way.
 */
static int full_sync(int fd) {
    if (fcntl(fd, F_FULLFSYNC) == 0) {
        return 0;
    }
    if (errno != ENOTSUP && errno != ENOTTY && errno != EINVAL) {
        return -1;
    }
    return fsync(fd);
}
#endif

/* Makes the data of a file durable. */
static int sync_file_data(int fd) {
#if defined(__APPLE__)
    return full_sync(fd);
#else
    return fdatasync(fd);
#endif
}

/* Makes the entries of a directory durable. */
static int sync_directory(int fd) {
#if defined(__APPLE__)
    return full_sync(fd);
#else
    return fsync(fd);
#endif
}

static atomic_bool direct_fallback_logged;

static void log_direct_fallback(const char* name) {
    if (!atomic_exchange(&direct_fallback_logged, true)) {
        SX_LOG_WARN("os",
                    "the file system does not support direct IO (file \"%s\"); "
                    "using buffered IO",
                    name);
    }
}

/*
 * Asks for the page cache to be bypassed on an open file. On Linux O_DIRECT is set
 * afterwards with F_SETFL rather than passed to open: some file systems refuse O_DIRECT
 * only after open(O_CREAT) has already created the file. Returns false if the file system
 * does not support it; the file then stays usable with buffered IO.
 */
static bool enable_direct_io(int fd) {
#if defined(__linux__)
    int status = fcntl(fd, F_GETFL);
    if (status < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, status | O_DIRECT) == 0;
#elif defined(__APPLE__)
    return fcntl(fd, F_NOCACHE, 1) == 0;
#else
    SX_UNUSED(fd);
    return false;
#endif
}

static bool is_aligned_transfer(const void* buffer, size_t size, uint64_t offset) {
    uint64_t mask = SX_FILE_DIRECT_ALIGNMENT - 1;
    return ((uintptr_t)buffer & mask) == 0 && (size & mask) == 0 && (offset & mask) == 0;
}

/* Returns "<base>/<name>" in newly allocated memory, or NULL. */
static char* join_path(const char* base, const char* name) {
    size_t length = strlen(base) + 1 + strlen(name) + 1;
    char* path = malloc(length);
    if (path != NULL) {
        snprintf(path, length, "%s/%s", base, name);
    }
    return path;
}

/* ---------------------------------------------------------------------------------------- */
/* Directories                                                                              */
/* ---------------------------------------------------------------------------------------- */

/* Wraps an open directory descriptor. Takes ownership of `fd` and `path`. */
static sx_err make_dir_handle(int fd, char* path, sx_dir** dir) {
    sx_dir* created = calloc(1, sizeof(*created));
    if (created == NULL || path == NULL) {
        free(created);
        free(path);
        close(fd);
        return SX_ERR_NOMEM;
    }
    created->fd = fd;
    created->path = path;
    *dir = created;
    return SX_OK;
}

sx_err sx_dir_open_path(const char* path, sx_dir** dir) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return sx_os_fail("open directory", path, errno);
    }
    return make_dir_handle(fd, strdup(path), dir);
}

sx_err sx_dir_open(sx_dir* parent, const char* name, sx_dir** dir) {
    if (!is_valid_name(name)) {
        return sx_os_fail("open directory", name, EINVAL);
    }
    int fd = openat(parent->fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return sx_os_fail("open directory", name, errno);
    }
    return make_dir_handle(fd, join_path(parent->path, name), dir);
}

void sx_dir_close(sx_dir* dir) {
    if (dir == NULL) {
        return;
    }
    close(dir->fd);
    free(dir->path);
    free(dir);
}

const char* sx_dir_path(const sx_dir* dir) {
    return dir->path;
}

sx_err sx_dir_create_path(const char* path) {
    /* Split into parent and last component, ignoring trailing slashes. */
    char buffer[PATH_SIZE];
    int written = snprintf(buffer, sizeof(buffer), "%s", path);
    if (written <= 0 || (size_t)written >= sizeof(buffer)) {
        return sx_os_fail("create directory", path, ENAMETOOLONG);
    }
    size_t length = (size_t)written;
    while (length > 1 && buffer[length - 1] == '/') {
        buffer[--length] = '\0';
    }
    char* slash = strrchr(buffer, '/');
    const char* parent_path = ".";
    const char* name = buffer;
    if (slash == buffer) {
        parent_path = "/";
        name = buffer + 1;
    } else if (slash != NULL) {
        *slash = '\0';
        parent_path = buffer;
        name = slash + 1;
    }
    sx_dir* parent = NULL;
    sx_err err = sx_dir_open_path(parent_path, &parent);
    if (err != SX_OK) {
        return err;
    }
    err = sx_dir_create(parent, name);
    sx_dir_close(parent);
    return err;
}

sx_err sx_dir_create(sx_dir* parent, const char* name) {
    if (!is_valid_name(name)) {
        return sx_os_fail("create directory", name, EINVAL);
    }
    if (sx_fault_is_armed()) {
        sx_err err = sx_fault_on_create(parent, name, "create directory");
        if (err != SX_OK) {
            return err;
        }
    }
    if (mkdirat(parent->fd, name, 0700) != 0) {
        return sx_os_fail("create directory", name, errno);
    }
    if (sx_fault_is_armed()) {
        sx_fault_created(parent, name, true);
    }
    return SX_OK;
}

sx_err sx_dir_sync(sx_dir* dir) {
    if (sx_fault_is_armed()) {
        sx_err err = sx_fault_on_dir_sync(dir);
        if (err != SX_OK) {
            return err;
        }
    }
    if (sync_directory(dir->fd) != 0) {
        return sx_os_fail("sync directory", dir->path, errno);
    }
    if (sx_fault_is_armed()) {
        sx_fault_dir_synced(dir);
    }
    return SX_OK;
}

sx_err sx_dir_rename(sx_dir* dir, const char* from, const char* to) {
    if (!is_valid_name(from) || !is_valid_name(to)) {
        return sx_os_fail("rename file", from, EINVAL);
    }
    if (sx_fault_is_armed()) {
        return sx_fault_rename(dir, from, to);
    }
    if (renameat(dir->fd, from, dir->fd, to) != 0) {
        return sx_os_fail("rename file", from, errno);
    }
    return SX_OK;
}

sx_err sx_dir_remove_file(sx_dir* dir, const char* name) {
    if (!is_valid_name(name)) {
        return sx_os_fail("remove file", name, EINVAL);
    }
    if (sx_fault_is_armed()) {
        return sx_fault_remove(dir, name);
    }
    if (unlinkat(dir->fd, name, 0) != 0) {
        return sx_os_fail("remove file", name, errno);
    }
    return SX_OK;
}

/* True if the entry is a directory, asking the file system when readdir does not say. */
static bool entry_is_dir(int dir_fd, const struct dirent* entry) {
    if (entry->d_type == DT_DIR) {
        return true;
    }
    if (entry->d_type != DT_UNKNOWN) {
        return false;
    }
    struct stat info;
    if (fstatat(dir_fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        return false;
    }
    return S_ISDIR(info.st_mode);
}

sx_err sx_dir_list(sx_dir* dir, sx_dir_entry_fn fn, void* context) {
    /* A fresh descriptor, so the listing has its own position in the directory. */
    int fd = openat(dir->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return sx_os_fail("list directory", dir->path, errno);
    }
    DIR* stream = fdopendir(fd);
    if (stream == NULL) {
        int saved = errno;
        close(fd);
        return sx_os_fail("list directory", dir->path, saved);
    }
    sx_err err = SX_OK;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(stream);
        if (entry == NULL) {
            if (errno != 0) {
                err = sx_os_fail("list directory", dir->path, errno);
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        err = fn(context, entry->d_name, entry_is_dir(dir->fd, entry));
        if (err != SX_OK) {
            break;
        }
    }
    closedir(stream);
    return err;
}

sx_err sx_dir_get_info(sx_dir* dir, sx_dir_info* info) {
    struct stat status;
    if (fstat(dir->fd, &status) != 0) {
        return sx_os_fail("stat directory", dir->path, errno);
    }
    info->owner_uid = (uint32_t)status.st_uid;
    info->mode = (uint32_t)(status.st_mode & 07777);
    return SX_OK;
}

sx_err sx_dir_replace_file(sx_dir* dir, const char* name, const void* data, size_t size) {
    char temp_name[SX_FILE_NAME_MAX + 1];
    int written = snprintf(temp_name, sizeof(temp_name), "%s.tmp", name);
    if (written < 0 || (size_t)written >= sizeof(temp_name) || !is_valid_name(name)) {
        return sx_os_fail("replace file", name, EINVAL);
    }
    /* A leftover of an interrupted replacement would make the create fail. */
    sx_err err = sx_dir_remove_file(dir, temp_name);
    if (err != SX_OK && err != SX_ERR_NOT_FOUND) {
        return err;
    }
    sx_file* file = NULL;
    err = sx_file_open(dir, temp_name, SX_FILE_WRITE | SX_FILE_CREATE, &file);
    if (err != SX_OK) {
        return err;
    }
    err = sx_file_write_at(file, data, size, 0);
    if (err == SX_OK) {
        err = sx_file_sync(file);
    }
    sx_file_close(file);
    if (err != SX_OK) {
        return err;
    }
    err = sx_dir_rename(dir, temp_name, name);
    if (err != SX_OK) {
        return err;
    }
    return sx_dir_sync(dir);
}

sx_err sx_dir_read_file(sx_dir* dir, const char* name, void* buffer, size_t capacity,
                        size_t* size) {
    sx_file* file = NULL;
    sx_err err = sx_file_open(dir, name, 0, &file);
    if (err != SX_OK) {
        return err;
    }
    uint64_t file_size = 0;
    err = sx_file_get_size(file, &file_size);
    if (err == SX_OK && file_size > capacity) {
        err = SX_ERR_LIMIT;
    }
    if (err == SX_OK) {
        err = sx_file_read_at(file, buffer, (size_t)file_size, 0);
    }
    if (err == SX_OK) {
        *size = (size_t)file_size;
    }
    sx_file_close(file);
    return err;
}

/* ---------------------------------------------------------------------------------------- */
/* Files                                                                                    */
/* ---------------------------------------------------------------------------------------- */

sx_err sx_file_open(sx_dir* dir, const char* name, unsigned flags, sx_file** file) {
    bool create = (flags & SX_FILE_CREATE) != 0;
    bool writable = create || (flags & SX_FILE_WRITE) != 0;
    const char* operation = create ? "create file" : "open file";
    if (!is_valid_name(name)) {
        return sx_os_fail(operation, name, EINVAL);
    }
    int open_flags = O_CLOEXEC | (writable ? O_RDWR : O_RDONLY);
    if (create) {
        open_flags |= O_CREAT | O_EXCL;
        if (sx_fault_is_armed()) {
            sx_err err = sx_fault_on_create(dir, name, operation);
            if (err != SX_OK) {
                return err;
            }
        }
    }
    int fd = openat(dir->fd, name, open_flags, 0600);
    if (fd < 0) {
        return sx_os_fail(operation, name, errno);
    }
    if (create && sx_fault_is_armed()) {
        sx_fault_created(dir, name, false);
    }
    sx_file* opened = calloc(1, sizeof(*opened));
    if (opened == NULL) {
        close(fd);
        return SX_ERR_NOMEM;
    }
    opened->fd = fd;
    opened->writable = writable;
    opened->aligned = (flags & SX_FILE_DIRECT) != 0;
    if (opened->aligned) {
        opened->direct = enable_direct_io(fd);
        if (!opened->direct) {
            log_direct_fallback(name);
        }
    }
    snprintf(opened->name, sizeof(opened->name), "%s", name);
    *file = opened;
    return SX_OK;
}

void sx_file_close(sx_file* file) {
    if (file == NULL) {
        return;
    }
    close(file->fd);
    free(file);
}

const char* sx_file_name(const sx_file* file) {
    return file->name;
}

bool sx_file_is_direct(const sx_file* file) {
    return file->direct;
}

sx_err sx_file_read_at(sx_file* file, void* buffer, size_t size, uint64_t offset) {
    if (file->aligned && !is_aligned_transfer(buffer, size, offset)) {
        return sx_os_fail("read file", file->name, EINVAL);
    }
    uint8_t* bytes = buffer;
    while (size > 0) {
        ssize_t got = pread(file->fd, bytes, size, (off_t)offset);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return sx_os_fail("read file", file->name, errno);
        }
        if (got == 0) {
            return fail_end_of_file(file->name);
        }
        bytes += got;
        size -= (size_t)got;
        offset += (uint64_t)got;
    }
    return SX_OK;
}

/* Writes all bytes, retrying short writes and interrupts. */
static sx_err write_fully(sx_file* file, const uint8_t* bytes, size_t size, uint64_t offset) {
    while (size > 0) {
        ssize_t written = pwrite(file->fd, bytes, size, (off_t)offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return sx_os_fail("write file", file->name, errno);
        }
        if (written == 0) {
            return sx_os_fail("write file", file->name, EIO);
        }
        bytes += written;
        size -= (size_t)written;
        offset += (uint64_t)written;
    }
    return SX_OK;
}

sx_err sx_file_write_at(sx_file* file, const void* data, size_t size, uint64_t offset) {
    if (!file->writable) {
        return sx_os_fail("write file", file->name, EBADF);
    }
    if (file->aligned && !is_aligned_transfer(data, size, offset)) {
        return sx_os_fail("write file", file->name, EINVAL);
    }
    if (sx_fault_is_armed()) {
        sx_err err = sx_fault_on_write(file, data, size, offset);
        if (err != SX_OK) {
            return err;
        }
    }
    return write_fully(file, data, size, offset);
}

sx_err sx_file_sync(sx_file* file) {
    if (sx_fault_is_armed()) {
        sx_err err = sx_fault_on_sync(file);
        if (err != SX_OK) {
            return err;
        }
    }
    if (sync_file_data(file->fd) != 0) {
        return sx_os_fail("sync file", file->name, errno);
    }
    if (sx_fault_is_armed()) {
        sx_fault_synced(file);
    }
    return SX_OK;
}

sx_err sx_file_get_size(sx_file* file, uint64_t* size) {
    struct stat status;
    if (fstat(file->fd, &status) != 0) {
        return sx_os_fail("stat file", file->name, errno);
    }
    *size = (uint64_t)status.st_size;
    return SX_OK;
}

sx_err sx_file_truncate(sx_file* file, uint64_t size) {
    if (sx_fault_is_armed()) {
        sx_err err = sx_fault_on_truncate(file, size);
        if (err != SX_OK) {
            return err;
        }
    }
    while (ftruncate(file->fd, (off_t)size) != 0) {
        if (errno != EINTR) {
            return sx_os_fail("truncate file", file->name, errno);
        }
    }
    return SX_OK;
}

/* Allocates blocks for the file up to `size` bytes and extends it to that size. */
static int allocate_blocks(int fd, uint64_t current_size, uint64_t size) {
#if defined(__APPLE__)
    fstore_t store = {
        .fst_flags = F_ALLOCATECONTIG | F_ALLOCATEALL,
        .fst_posmode = F_PEOFPOSMODE,
        .fst_offset = 0,
        .fst_length = (off_t)(size - current_size),
    };
    if (fcntl(fd, F_PREALLOCATE, &store) != 0) {
        /* No contiguous run of that size; any blocks will do. */
        store.fst_flags = F_ALLOCATEALL;
        if (fcntl(fd, F_PREALLOCATE, &store) != 0) {
            return errno;
        }
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        return errno;
    }
    return 0;
#else
    SX_UNUSED(current_size);
    int rc;
    do {
        rc = posix_fallocate(fd, 0, (off_t)size);
    } while (rc == EINTR);
    return rc;
#endif
}

sx_err sx_file_preallocate(sx_file* file, uint64_t size) {
    uint64_t current_size = 0;
    sx_err err = sx_file_get_size(file, &current_size);
    if (err != SX_OK || current_size >= size) {
        return err;
    }
    if (sx_fault_is_armed()) {
        err = sx_fault_on_preallocate(file, size);
        if (err != SX_OK) {
            return err;
        }
    }
    int rc = allocate_blocks(file->fd, current_size, size);
    if (rc != 0) {
        return sx_os_fail("preallocate file", file->name, rc);
    }
    return SX_OK;
}

sx_err sx_file_try_lock(sx_file* file) {
    while (flock(file->fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno != EINTR) {
            return sx_os_fail("lock file", file->name, errno);
        }
    }
    return SX_OK;
}
