/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Fast non-cryptographic hashing for in-memory hash tables. The output is not stable
 * across releases and must never be stored on disk.
 */

#ifndef SX_COMMON_HASH_H
#define SX_COMMON_HASH_H

#include <stddef.h>
#include <stdint.h>

/* Mixes all bits of x into all bits of the result. */
static inline uint64_t sx_hash_u64(uint64_t x) {
    x ^= x >> 32;
    x *= UINT64_C(0xD6E8FEB86659FD93);
    x ^= x >> 32;
    x *= UINT64_C(0xD6E8FEB86659FD93);
    x ^= x >> 32;
    return x;
}

/* Hashes `size` bytes of `data`. */
uint64_t sx_hash_bytes(const void* data, size_t size, uint64_t seed);

#endif /* SX_COMMON_HASH_H */
