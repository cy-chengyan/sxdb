/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Small bit-manipulation and unaligned load/store helpers.
 *
 * Loads and stores go through memcpy, which is well defined for any alignment and
 * compiles to a single instruction on the supported platforms. All multi-byte values
 * are little-endian, which matches the in-memory order on every supported platform.
 */

#ifndef SX_COMMON_BITS_H
#define SX_COMMON_BITS_H

#include "common/compiler.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Number of bits needed to represent x; 0 for x == 0. Branch-free, because codecs call it
 * on data where zero and non-zero values alternate unpredictably.
 */
static inline uint32_t sx_bit_width_u64(uint64_t x) {
    return 64u - (uint32_t)__builtin_clzll(x | 1u) - (uint32_t)(x == 0);
}

/* Number of trailing zero bits; x must not be 0. */
static inline uint32_t sx_trailing_zeros_u64(uint64_t x) {
    return (uint32_t)__builtin_ctzll(x);
}

/* Number of set bits. */
static inline uint32_t sx_popcount_u64(uint64_t x) {
    return (uint32_t)__builtin_popcountll(x);
}

/* A mask with the lowest `width` bits set; width may be 0 to 64. */
static inline uint64_t sx_low_mask_u64(uint32_t width) {
    if (width >= 64) {
        return UINT64_MAX;
    }
    return (UINT64_C(1) << width) - 1u;
}

/*
 * Greatest common divisor (binary algorithm). gcd(0, b) is b. With both numbers odd, each
 * step replaces the larger one by their difference without its factors of two. The
 * trailing zeros are counted on b - a, which has as many as |a - b|, so the count does
 * not wait for the comparison that picks the larger number.
 */
static inline uint64_t sx_gcd_u64(uint64_t a, uint64_t b) {
    if (a == 0) {
        return b;
    }
    if (b == 0) {
        return a;
    }
    if (a == 1 || b == 1) {
        return 1; /* common in codecs, and slow for the loop below */
    }
    uint32_t shift = sx_trailing_zeros_u64(a | b);
    a >>= sx_trailing_zeros_u64(a);
    b >>= sx_trailing_zeros_u64(b);
    while (a != b) {
        uint32_t zeros = sx_trailing_zeros_u64(b - a);
        uint64_t smaller = a < b ? a : b;
        uint64_t difference = a < b ? b - a : a - b;
        a = smaller;
        b = difference >> zeros;
    }
    return a << shift;
}

static inline size_t sx_align_up(size_t value, size_t alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}

static inline size_t sx_div_round_up(size_t value, size_t divisor) {
    return (value + divisor - 1u) / divisor;
}

/* Zigzag encoding maps signed integers to unsigned ones so small magnitudes stay small. */
static inline uint64_t sx_zigzag_encode(int64_t value) {
    return ((uint64_t)value << 1) ^ (uint64_t)(value >> 63);
}

static inline int64_t sx_zigzag_decode(uint64_t value) {
    uint64_t sign_mask = 0u - (value & 1u);
    return (int64_t)((value >> 1) ^ sign_mask);
}

static inline uint16_t sx_load_u16(const void* src) {
    uint16_t value;
    memcpy(&value, src, sizeof(value));
    return value;
}

static inline uint32_t sx_load_u32(const void* src) {
    uint32_t value;
    memcpy(&value, src, sizeof(value));
    return value;
}

static inline uint64_t sx_load_u64(const void* src) {
    uint64_t value;
    memcpy(&value, src, sizeof(value));
    return value;
}

static inline void sx_store_u16(void* dst, uint16_t value) {
    memcpy(dst, &value, sizeof(value));
}

static inline void sx_store_u32(void* dst, uint32_t value) {
    memcpy(dst, &value, sizeof(value));
}

static inline void sx_store_u64(void* dst, uint64_t value) {
    memcpy(dst, &value, sizeof(value));
}

/* Reinterprets the bits of a double as an unsigned integer and back. */
static inline uint64_t sx_double_to_bits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static inline double sx_bits_to_double(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

#endif /* SX_COMMON_BITS_H */
