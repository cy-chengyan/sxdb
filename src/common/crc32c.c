/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "common/crc32c.h"

#include "common/bits.h"
#include "common/compiler.h"
#include "os/os.h"

#include <pthread.h>

#if defined(SX_ARCH_X86_64)
#include <nmmintrin.h>
#elif defined(SX_ARCH_ARM64)
#include <arm_acle.h>
#endif

/* Reflected Castagnoli polynomial. */
#define CRC32C_POLY 0x82F63B78u

typedef uint32_t (*crc32c_fn)(uint32_t crc, const uint8_t* data, size_t size);

/*
 * Slicing-by-8 tables: table[k][b] is the CRC of byte b followed by k zero bytes.
 * Filled once by init_tables().
 */
static uint32_t crc_table[8][256];
static crc32c_fn crc_impl;
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void init_tables(void) {
    for (uint32_t byte = 0; byte < 256; byte++) {
        uint32_t crc = byte;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ CRC32C_POLY;
            } else {
                crc >>= 1;
            }
        }
        crc_table[0][byte] = crc;
    }
    for (uint32_t byte = 0; byte < 256; byte++) {
        uint32_t crc = crc_table[0][byte];
        for (int k = 1; k < 8; k++) {
            crc = crc_table[0][crc & 0xFFu] ^ (crc >> 8);
            crc_table[k][byte] = crc;
        }
    }
}

static uint32_t crc32c_software_raw(uint32_t crc, const uint8_t* data, size_t size) {
    while (size >= 8) {
        uint64_t word = sx_load_u64(data) ^ crc;
        crc = crc_table[7][word & 0xFFu] ^ crc_table[6][(word >> 8) & 0xFFu] ^
              crc_table[5][(word >> 16) & 0xFFu] ^ crc_table[4][(word >> 24) & 0xFFu] ^
              crc_table[3][(word >> 32) & 0xFFu] ^ crc_table[2][(word >> 40) & 0xFFu] ^
              crc_table[1][(word >> 48) & 0xFFu] ^ crc_table[0][word >> 56];
        data += 8;
        size -= 8;
    }
    while (size > 0) {
        crc = crc_table[0][(crc ^ *data) & 0xFFu] ^ (crc >> 8);
        data++;
        size--;
    }
    return crc;
}

#if defined(SX_ARCH_X86_64)

__attribute__((target("sse4.2"))) static uint32_t crc32c_hardware_raw(uint32_t crc,
                                                                       const uint8_t* data,
                                                                       size_t size) {
    uint64_t crc64 = crc;
    while (size >= 8) {
        crc64 = _mm_crc32_u64(crc64, sx_load_u64(data));
        data += 8;
        size -= 8;
    }
    crc = (uint32_t)crc64;
    while (size > 0) {
        crc = _mm_crc32_u8(crc, *data);
        data++;
        size--;
    }
    return crc;
}

#elif defined(SX_ARCH_ARM64)

/* Apple targets always have the CRC extension; other ARMv8 targets need it enabled. */
#if defined(__ARM_FEATURE_CRC32)
#define SX_TARGET_CRC
#elif defined(__clang__)
#define SX_TARGET_CRC __attribute__((target("crc")))
#else
#define SX_TARGET_CRC __attribute__((target("+crc")))
#endif

SX_TARGET_CRC static uint32_t crc32c_hardware_raw(uint32_t crc, const uint8_t* data,
                                                  size_t size) {
    while (size >= 8) {
        crc = __crc32cd(crc, sx_load_u64(data));
        data += 8;
        size -= 8;
    }
    while (size > 0) {
        crc = __crc32cb(crc, *data);
        data++;
        size--;
    }
    return crc;
}

#endif

static void init_crc32c(void) {
    init_tables();
    if (sx_os_cpu_has_crc32c()) {
        crc_impl = crc32c_hardware_raw;
    } else {
        crc_impl = crc32c_software_raw;
    }
}

uint32_t sx_crc32c(uint32_t crc, const void* data, size_t size) {
    pthread_once(&crc_once, init_crc32c);
    return ~crc_impl(~crc, data, size);
}

uint32_t sx_crc32c_software(uint32_t crc, const void* data, size_t size) {
    pthread_once(&crc_once, init_crc32c);
    return ~crc32c_software_raw(~crc, data, size);
}
