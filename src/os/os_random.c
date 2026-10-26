/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "os/os.h"

#include <errno.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/random.h>
#endif

/* getentropy returns at most this many bytes per call. */
#define ENTROPY_CHUNK 256

sx_err sx_os_random_bytes(void* buffer, size_t size) {
    unsigned char* bytes = buffer;
    while (size > 0) {
        size_t chunk = size < ENTROPY_CHUNK ? size : ENTROPY_CHUNK;
        if (getentropy(bytes, chunk) != 0) {
            if (errno == EINTR) {
                continue;
            }
            return SX_ERR_IO;
        }
        bytes += chunk;
        size -= chunk;
    }
    return SX_OK;
}
