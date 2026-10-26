/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "common/xxhash.h"

#include <string.h>

#define PRIME64_1 UINT64_C(0x9E3779B185EBCA87)
#define PRIME64_2 UINT64_C(0xC2B2AE3D27D4EB4F)
#define PRIME64_3 UINT64_C(0x165667B19E3779F9)
#define PRIME64_4 UINT64_C(0x85EBCA77C2B2AE63)
#define PRIME64_5 UINT64_C(0x27D4EB2F165667C5)

static inline uint64_t rotate_left(uint64_t value, unsigned bits) {
    return (value << bits) | (value >> (64 - bits));
}

/* Unaligned little-endian loads (the platform is little-endian, see common/compiler.h). */
static inline uint64_t load_u64(const uint8_t* bytes) {
    uint64_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static inline uint32_t load_u32(const uint8_t* bytes) {
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

/* Folds one 8-byte lane into an accumulator. */
static inline uint64_t round_lane(uint64_t accumulator, uint64_t lane) {
    accumulator += lane * PRIME64_2;
    accumulator = rotate_left(accumulator, 31);
    return accumulator * PRIME64_1;
}

/* Merges one of the four stripe accumulators into the hash. */
static inline uint64_t merge_accumulator(uint64_t hash, uint64_t accumulator) {
    hash ^= round_lane(0, accumulator);
    return hash * PRIME64_1 + PRIME64_4;
}

/* Consumes all 32-byte stripes; returns the hash of their accumulators. */
static uint64_t hash_stripes(const uint8_t** cursor, const uint8_t* end, uint64_t seed) {
    uint64_t acc1 = seed + PRIME64_1 + PRIME64_2;
    uint64_t acc2 = seed + PRIME64_2;
    uint64_t acc3 = seed;
    uint64_t acc4 = seed - PRIME64_1;
    const uint8_t* bytes = *cursor;
    while (end - bytes >= 32) {
        acc1 = round_lane(acc1, load_u64(bytes));
        acc2 = round_lane(acc2, load_u64(bytes + 8));
        acc3 = round_lane(acc3, load_u64(bytes + 16));
        acc4 = round_lane(acc4, load_u64(bytes + 24));
        bytes += 32;
    }
    *cursor = bytes;
    uint64_t hash =
        rotate_left(acc1, 1) + rotate_left(acc2, 7) + rotate_left(acc3, 12) + rotate_left(acc4, 18);
    hash = merge_accumulator(hash, acc1);
    hash = merge_accumulator(hash, acc2);
    hash = merge_accumulator(hash, acc3);
    return merge_accumulator(hash, acc4);
}

uint64_t sx_xxh64(const void* data, size_t size, uint64_t seed) {
    const uint8_t* bytes = data;
    const uint8_t* end = bytes + size;
    uint64_t hash;
    if (size >= 32) {
        hash = hash_stripes(&bytes, end, seed);
    } else {
        hash = seed + PRIME64_5;
    }
    hash += (uint64_t)size;

    while (end - bytes >= 8) {
        hash ^= round_lane(0, load_u64(bytes));
        hash = rotate_left(hash, 27) * PRIME64_1 + PRIME64_4;
        bytes += 8;
    }
    if (end - bytes >= 4) {
        hash ^= (uint64_t)load_u32(bytes) * PRIME64_1;
        hash = rotate_left(hash, 23) * PRIME64_2 + PRIME64_3;
        bytes += 4;
    }
    while (bytes < end) {
        hash ^= (uint64_t)(*bytes) * PRIME64_5;
        hash = rotate_left(hash, 11) * PRIME64_1;
        bytes++;
    }

    /* Final avalanche. */
    hash ^= hash >> 33;
    hash *= PRIME64_2;
    hash ^= hash >> 29;
    hash *= PRIME64_3;
    hash ^= hash >> 32;
    return hash;
}
