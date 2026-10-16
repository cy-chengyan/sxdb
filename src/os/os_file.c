/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "os/os.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

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
