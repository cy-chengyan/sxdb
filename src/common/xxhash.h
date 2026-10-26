/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * XXH64, a fast non-cryptographic hash with a published specification and reference test
 * vectors. Written in-house from the specification.
 *
 * Unlike the in-memory hash in common/hash.h, its output is frozen: on-disk structures
 * such as Bloom filters (docs/DESIGN.md section 5.5) store values derived from it, so it
 * must never change.
 */

#ifndef SX_COMMON_XXHASH_H
#define SX_COMMON_XXHASH_H

#include <stddef.h>
#include <stdint.h>

/* Hashes `size` bytes of `data` with `seed`. */
uint64_t sx_xxh64(const void* data, size_t size, uint64_t seed);

#endif /* SX_COMMON_XXHASH_H */
