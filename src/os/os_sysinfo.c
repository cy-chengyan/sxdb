/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "os/os.h"

#include "common/compiler.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#if defined(SX_ARCH_X86_64)
#include <cpuid.h>
#endif

#if defined(__linux__) && defined(SX_ARCH_ARM64)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

uint64_t sx_os_physical_memory(void) {
#if defined(__APPLE__)
    uint64_t bytes = 0;
    size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, NULL, 0) != 0) {
        return 0;
    }
    return bytes;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0) {
        return 0;
    }
    return (uint64_t)pages * (uint64_t)page_size;
#endif
}

#if defined(__linux__)

/* Reads a cgroup limit file. "max" or anything unparsable means no limit (0). */
static uint64_t read_limit_file(const char* path) {
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }
    char text[64] = {0};
    char* line = fgets(text, sizeof(text), file);
    fclose(file);
    if (line == NULL || strncmp(text, "max", 3) == 0) {
        return 0;
    }
    unsigned long long value = 0;
    if (sscanf(text, "%llu", &value) != 1) {
        return 0;
    }
    /* cgroup v1 reports "no limit" as a huge page-aligned number. */
    if (value >= (UINT64_C(1) << 62)) {
        return 0;
    }
    return (uint64_t)value;
}

/* Finds this process's cgroup v2 path from /proc/self/cgroup ("0::/some/path"). */
static int read_cgroup_v2_path(char* path, size_t size) {
    FILE* file = fopen("/proc/self/cgroup", "r");
    if (file == NULL) {
        return -1;
    }
    char line[512];
    int found = -1;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "0::", 3) == 0) {
            line[strcspn(line, "\n")] = '\0';
            int written = snprintf(path, size, "%s", line + 3);
            if (written >= 0 && (size_t)written < size) {
                found = 0;
            }
            break;
        }
    }
    fclose(file);
    return found;
}

uint64_t sx_os_container_memory_limit(void) {
    char cgroup_path[400];
    char file_path[512];
    if (read_cgroup_v2_path(cgroup_path, sizeof(cgroup_path)) == 0) {
        snprintf(file_path, sizeof(file_path), "/sys/fs/cgroup%s/memory.max", cgroup_path);
        uint64_t limit = read_limit_file(file_path);
        if (limit != 0) {
            return limit;
        }
    }
    uint64_t limit = read_limit_file("/sys/fs/cgroup/memory.max");
    if (limit != 0) {
        return limit;
    }
    return read_limit_file("/sys/fs/cgroup/memory/memory.limit_in_bytes");
}

#else

uint64_t sx_os_container_memory_limit(void) {
    return 0;
}

#endif

uint32_t sx_os_cpu_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1) {
        return 1;
    }
    return (uint32_t)count;
}

size_t sx_os_page_size(void) {
    long size = sysconf(_SC_PAGESIZE);
    if (size <= 0) {
        return 4096;
    }
    return (size_t)size;
}

bool sx_os_cpu_has_crc32c(void) {
#if defined(SX_ARCH_X86_64)
    return __builtin_cpu_supports("sse4.2");
#elif defined(__APPLE__)
    return true;
#elif defined(__linux__)
    return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#else
    return false;
#endif
}

#if defined(SX_ARCH_X86_64)
/*
 * LZCNT is CPUID leaf 0x80000001, ECX bit 5. Unlike the other features it cannot be
 * tested with __builtin_cpu_supports on every supported compiler.
 */
static bool cpu_has_lzcnt(void) {
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid(0x80000001u, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    return (ecx & (1u << 5)) != 0;
}
#endif

/*
 * __builtin_cpu_supports also checks (with XGETBV) that the operating system saves the
 * vector registers, so a true result means the instructions can be used.
 */
bool sx_os_cpu_has_x86_64_v3(void) {
#if defined(SX_ARCH_X86_64)
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi") &&
           __builtin_cpu_supports("bmi2") && __builtin_cpu_supports("popcnt") && cpu_has_lzcnt();
#else
    return false;
#endif
}

bool sx_os_cpu_has_x86_64_v4(void) {
#if defined(SX_ARCH_X86_64)
    return sx_os_cpu_has_x86_64_v3() && __builtin_cpu_supports("avx512f") &&
           __builtin_cpu_supports("avx512cd") && __builtin_cpu_supports("avx512bw") &&
           __builtin_cpu_supports("avx512dq") && __builtin_cpu_supports("avx512vl");
#else
    return false;
#endif
}

uint32_t sx_os_effective_uid(void) {
    return (uint32_t)geteuid();
}

uint64_t sx_os_process_id(void) {
    return (uint64_t)getpid();
}
