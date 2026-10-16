/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "common/hash.h"

#include "common/bits.h"

#include <string.h>

#define GOLDEN_RATIO_64 UINT64_C(0x9E3779B97F4A7C15)

uint64_t sx_hash_bytes(const void* data, size_t size, uint64_t seed) {
    const uint8_t* bytes = data;
    uint64_t hash = seed ^ ((uint64_t)size * GOLDEN_RATIO_64);

    while (size >= 8) {
        hash = sx_hash_u64(hash ^ sx_load_u64(bytes)) + GOLDEN_RATIO_64;
        bytes += 8;
        size -= 8;
    }

    if (size > 0) {
        uint64_t tail = 0;
        memcpy(&tail, bytes, size);
        hash = sx_hash_u64(hash ^ tail ^ ((uint64_t)size << 59));
    }
    return sx_hash_u64(hash);
}
