/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Operating system interface: memory mapping, system information, and file helpers.
 * Everything platform specific lives behind these functions.
 */

#ifndef SX_OS_OS_H
#define SX_OS_OS_H

#include "common/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Maps `size` bytes of zero-filled, read-write anonymous memory. Returns NULL on failure. */
void* sx_os_map_anonymous(size_t size);

/* Unmaps memory returned by sx_os_map_anonymous. */
void sx_os_unmap(void* addr, size_t size);

/*
 * Tells the operating system that the contents of the range are no longer needed, so
 * the physical pages can be reclaimed. The range stays mapped; its contents become
 * undefined.
 */
void sx_os_discard_pages(void* addr, size_t size);

/* Total physical memory of the machine in bytes. */
uint64_t sx_os_physical_memory(void);

/* Memory limit of the enclosing container (cgroup), or 0 if there is none. */
uint64_t sx_os_container_memory_limit(void);

/* Number of online CPUs (at least 1). */
uint32_t sx_os_cpu_count(void);

/* Size of a virtual memory page in bytes. */
size_t sx_os_page_size(void);

/* True if the CPU has CRC32C instructions (SSE4.2 on x86_64, the CRC extension on ARM64). */
bool sx_os_cpu_has_crc32c(void);

/* True if the CPU and the operating system support AVX2 (x86_64 only). */
bool sx_os_cpu_has_avx2(void);

/* True if the CPU and the operating system support AVX-512 F and DQ (x86_64 only). */
bool sx_os_cpu_has_avx512(void);

/* Writes all `size` bytes to `fd`, retrying on short writes and interrupts. */
sx_err sx_os_write_all(int fd, const void* data, size_t size);

/* Flushes a directory entry change (create, rename, unlink) in `dir_path` to disk. */
sx_err sx_os_fsync_dir(const char* dir_path);

#endif /* SX_OS_OS_H */
