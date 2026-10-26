/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Split-block Bloom filters, as specified for Apache Parquet. See docs/DESIGN.md
 * section 5.5.
 *
 * A filter is an array of 256-bit blocks of eight 32-bit words. A key's 64-bit hash picks
 * one block with its high 32 bits (multiply-shift) and sets one bit in each of the eight
 * words with its low 32 bits, multiplied by eight fixed odd salts. A lookup therefore
 * reads one 32-byte block, within one cache line, and has no data-dependent branches.
 * At 10 bits per key about 1% of absent keys are reported as possibly present.
 *
 * Serialized layout, little-endian:
 *
 *   u32 block_count | u8 version (1) | u8 reserved[27] | block_count blocks of 32 bytes
 *
 * The 32-byte header keeps the blocks 32-byte aligned relative to the start of the filter.
 * A filter is built in a caller-provided buffer and read in place through a view.
 */

#ifndef SX_COMMON_BLOOM_H
#define SX_COMMON_BLOOM_H

#include "common/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SX_BLOOM_HEADER_SIZE  32u
#define SX_BLOOM_BLOCK_SIZE   32u
#define SX_BLOOM_BITS_PER_KEY 10u

/* A filter being built in a writable buffer. */
typedef struct sx_bloom {
    uint8_t* blocks;
    uint32_t block_count;
} sx_bloom;

/* A serialized filter read in place. */
typedef struct sx_bloom_view {
    const uint8_t* blocks;
    uint32_t block_count;
} sx_bloom_view;

/* Hash of a key for a Bloom filter: XXH64 with seed 0, frozen as part of the format. */
uint64_t sx_bloom_hash(const void* key, size_t size);

/* Serialized size of a filter for `key_count` keys (at least one block). */
size_t sx_bloom_size_for(uint64_t key_count);

/*
 * Starts an empty filter in `buffer`, whose `size` is SX_BLOOM_HEADER_SIZE plus a multiple
 * of SX_BLOOM_BLOCK_SIZE (as returned by sx_bloom_size_for).
 */
sx_err sx_bloom_init(sx_bloom* bloom, void* buffer, size_t size);

/* Adds a key by its hash. */
void sx_bloom_add(sx_bloom* bloom, uint64_t hash);

/* Opens a serialized filter of `size` bytes. SX_ERR_CORRUPT if its header does not match. */
sx_err sx_bloom_view_open(sx_bloom_view* view, const void* data, size_t size);

/* False if the key is certainly absent; true if it may be present. */
bool sx_bloom_may_contain(const sx_bloom_view* view, uint64_t hash);

#endif /* SX_COMMON_BLOOM_H */
