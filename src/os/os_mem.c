/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "os/os.h"

#include <sys/mman.h>

void* sx_os_map_anonymous(size_t size) {
    void* addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        return NULL;
    }
    return addr;
}

void sx_os_unmap(void* addr, size_t size) {
    if (addr != NULL) {
        munmap(addr, size);
    }
}

void sx_os_discard_pages(void* addr, size_t size) {
#if defined(__APPLE__)
    madvise(addr, size, MADV_FREE);
#else
    madvise(addr, size, MADV_DONTNEED);
#endif
}
