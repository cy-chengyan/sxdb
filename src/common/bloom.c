/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "common/bloom.h"

#include "common/xxhash.h"

#include <string.h>

#define BLOOM_VERSION   1u
#define WORDS_PER_BLOCK 8u

/* The salts of the Parquet split-block Bloom filter specification. */
static const uint32_t salts[WORDS_PER_BLOCK] = {
    0x47B6137Bu, 0x44974D91u, 0x8824AD5Bu, 0xA2B7289Du,
    0x705495C7u, 0x2DF1424Bu, 0x9EFC4947u, 0x5C6BFB31u,
};

uint64_t sx_bloom_hash(const void* key, size_t size) {
    return sx_xxh64(key, size, 0);
}

size_t sx_bloom_size_for(uint64_t key_count) {
    const uint64_t bits_per_block = (uint64_t)SX_BLOOM_BLOCK_SIZE * 8;
    uint64_t bits = key_count * SX_BLOOM_BITS_PER_KEY;
    uint64_t blocks = (bits + bits_per_block - 1) / bits_per_block;
    if (blocks == 0) {
        blocks = 1;
    }
    if (blocks > UINT32_MAX) {
        blocks = UINT32_MAX;
    }
    return SX_BLOOM_HEADER_SIZE + (size_t)blocks * SX_BLOOM_BLOCK_SIZE;
}

/* Picks the block of a hash: its high 32 bits scaled to the block count. */
static inline uint32_t block_index(uint64_t hash, uint32_t block_count) {
    return (uint32_t)(((hash >> 32) * (uint64_t)block_count) >> 32);
}

/* The bit each word of the block must have for this hash. */
static inline void make_mask(uint64_t hash, uint32_t mask[WORDS_PER_BLOCK]) {
    uint32_t key = (uint32_t)hash;
    for (unsigned i = 0; i < WORDS_PER_BLOCK; i++) {
        mask[i] = UINT32_C(1) << ((key * salts[i]) >> 27);
    }
}

sx_err sx_bloom_init(sx_bloom* bloom, void* buffer, size_t size) {
    if (size < SX_BLOOM_HEADER_SIZE + SX_BLOOM_BLOCK_SIZE ||
        (size - SX_BLOOM_HEADER_SIZE) % SX_BLOOM_BLOCK_SIZE != 0) {
        return SX_ERR_INVALID_ARG;
    }
    uint64_t block_count = (size - SX_BLOOM_HEADER_SIZE) / SX_BLOOM_BLOCK_SIZE;
    if (block_count > UINT32_MAX) {
        return SX_ERR_INVALID_ARG;
    }
    uint8_t* bytes = buffer;
    memset(bytes, 0, size);
    uint32_t count = (uint32_t)block_count;
    memcpy(bytes, &count, sizeof(count));
    bytes[4] = BLOOM_VERSION;
    bloom->blocks = bytes + SX_BLOOM_HEADER_SIZE;
    bloom->block_count = count;
    return SX_OK;
}

void sx_bloom_add(sx_bloom* bloom, uint64_t hash) {
    uint32_t mask[WORDS_PER_BLOCK];
    make_mask(hash, mask);
    uint8_t* block =
        bloom->blocks + (size_t)block_index(hash, bloom->block_count) * SX_BLOOM_BLOCK_SIZE;
    uint32_t words[WORDS_PER_BLOCK];
    memcpy(words, block, sizeof(words));
    for (unsigned i = 0; i < WORDS_PER_BLOCK; i++) {
        words[i] |= mask[i];
    }
    memcpy(block, words, sizeof(words));
}

sx_err sx_bloom_view_open(sx_bloom_view* view, const void* data, size_t size) {
    const uint8_t* bytes = data;
    if (size < SX_BLOOM_HEADER_SIZE + SX_BLOOM_BLOCK_SIZE) {
        return SX_ERR_CORRUPT;
    }
    uint32_t count;
    memcpy(&count, bytes, sizeof(count));
    if (bytes[4] != BLOOM_VERSION || count == 0 ||
        (uint64_t)(size - SX_BLOOM_HEADER_SIZE) != (uint64_t)count * SX_BLOOM_BLOCK_SIZE) {
        return SX_ERR_CORRUPT;
    }
    view->blocks = bytes + SX_BLOOM_HEADER_SIZE;
    view->block_count = count;
    return SX_OK;
}

bool sx_bloom_may_contain(const sx_bloom_view* view, uint64_t hash) {
    uint32_t mask[WORDS_PER_BLOCK];
    make_mask(hash, mask);
    const uint8_t* block =
        view->blocks + (size_t)block_index(hash, view->block_count) * SX_BLOOM_BLOCK_SIZE;
    uint32_t words[WORDS_PER_BLOCK];
    memcpy(words, block, sizeof(words));
    uint32_t missing = 0;
    for (unsigned i = 0; i < WORDS_PER_BLOCK; i++) {
        missing |= ~words[i] & mask[i];
    }
    return missing == 0;
}
