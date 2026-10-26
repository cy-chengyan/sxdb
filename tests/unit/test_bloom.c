/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sx_test.h"

#include "common/bits.h"
#include "common/bloom.h"
#include "common/compiler.h"
#include "common/xxhash.h"

#include <stdlib.h>
#include <string.h>

typedef struct xxh64_vector {
    size_t size;
    uint64_t seed0;
    uint64_t seed_golden; /* with seed 0x9E3779B97F4A7C15 */
} xxh64_vector;

/* Inputs: "", "a", "abc", the sentence below, bytes 0..31, 0..99, and (7i+3) mod 256. */
static const char sentence[] = "Nobody inspects the spammish repetition";

static void fill_input(uint8_t* bytes, size_t index, size_t size) {
    for (size_t i = 0; i < size; i++) {
        bytes[i] = index == 6 ? (uint8_t)((i * 7 + 3) & 255) : (uint8_t)i;
    }
}

/* Reference values from the xxHash project's implementation. */
static void test_xxh64_reference_vectors(void) {
    static const xxh64_vector vectors[] = {
        {0, UINT64_C(0xef46db3751d8e999), UINT64_C(0xc4349fc93c010000)},
        {1, UINT64_C(0xd24ec4f1a98c6e5b), UINT64_C(0x9a7c6d2ea45568c9)},
        {3, UINT64_C(0x44bc2cf5ad770999), UINT64_C(0x2ed0f59d6b43ac8b)},
        {39, UINT64_C(0xfbcea83c8a378bf1), UINT64_C(0xeb8b157ca26cbf34)},
        {32, UINT64_C(0xcbf59c5116ff32b4), UINT64_C(0xa1c89217e9d50750)},
        {100, UINT64_C(0x6ac1e58032166597), UINT64_C(0x3b97d91eba03e785)},
        {1000, UINT64_C(0x5f235fa033f1a3fb), UINT64_C(0x442acd0a822e86f6)},
    };
    const uint64_t golden_seed = UINT64_C(0x9E3779B97F4A7C15);
    uint8_t input[1000];
    for (size_t i = 0; i < SX_ARRAY_SIZE(vectors); i++) {
        const xxh64_vector* vector = &vectors[i];
        if (i == 1) {
            input[0] = 'a';
        } else if (i == 2) {
            input[0] = 'a';
            input[1] = 'b';
            input[2] = 'c';
        } else if (i == 3) {
            memcpy(input, sentence, vector->size);
        } else {
            fill_input(input, i, vector->size);
        }
        SX_CHECK_EQ_U64(sx_xxh64(input, vector->size, 0), vector->seed0);
        SX_CHECK_EQ_U64(sx_xxh64(input, vector->size, golden_seed), vector->seed_golden);
    }
}

static void test_xxh64_ignores_alignment(void) {
    uint8_t buffer[80];
    for (size_t i = 0; i < sizeof(buffer); i++) {
        buffer[i] = (uint8_t)(i * 13);
    }
    uint64_t expected = sx_xxh64(buffer, 64, 5);
    uint8_t shifted[81];
    memcpy(shifted + 1, buffer, 64);
    SX_CHECK_EQ_U64(sx_xxh64(shifted + 1, 64, 5), expected);
}

static void test_bloom_sizes(void) {
    SX_CHECK_EQ_U64(sx_bloom_size_for(0), SX_BLOOM_HEADER_SIZE + SX_BLOOM_BLOCK_SIZE);
    SX_CHECK_EQ_U64(sx_bloom_size_for(25), SX_BLOOM_HEADER_SIZE + SX_BLOOM_BLOCK_SIZE);
    SX_CHECK_EQ_U64(sx_bloom_size_for(26), SX_BLOOM_HEADER_SIZE + 2 * SX_BLOOM_BLOCK_SIZE);
    SX_CHECK_EQ_U64(sx_bloom_size_for(1000000),
                    SX_BLOOM_HEADER_SIZE + 39063 * (uint64_t)SX_BLOOM_BLOCK_SIZE);

    sx_bloom bloom;
    uint8_t buffer[128];
    SX_CHECK(sx_bloom_init(&bloom, buffer, 63) == SX_ERR_INVALID_ARG);
    SX_CHECK(sx_bloom_init(&bloom, buffer, 70) == SX_ERR_INVALID_ARG);
    SX_CHECK(sx_bloom_init(&bloom, buffer, 96) == SX_OK);
    SX_CHECK_EQ_U64(bloom.block_count, 2);
}

/* No false negatives, and the false positive rate is near 1%. */
static void test_bloom_membership_and_rate(void) {
    const uint64_t key_count = 100000;
    size_t size = sx_bloom_size_for(key_count);
    uint8_t* buffer = malloc(size);
    SX_CHECK(buffer != NULL);
    sx_bloom bloom;
    SX_CHECK(sx_bloom_init(&bloom, buffer, size) == SX_OK);
    for (uint64_t key = 0; key < key_count; key++) {
        sx_bloom_add(&bloom, sx_bloom_hash(&key, sizeof(key)));
    }

    sx_bloom_view view;
    SX_CHECK(sx_bloom_view_open(&view, buffer, size) == SX_OK);
    SX_CHECK_EQ_U64(view.block_count, bloom.block_count);
    for (uint64_t key = 0; key < key_count; key++) {
        if (!sx_bloom_may_contain(&view, sx_bloom_hash(&key, sizeof(key)))) {
            free(buffer);
            SX_CHECK(false);
        }
    }
    uint64_t false_positives = 0;
    const uint64_t probes = 300000;
    for (uint64_t key = key_count; key < key_count + probes; key++) {
        if (sx_bloom_may_contain(&view, sx_bloom_hash(&key, sizeof(key)))) {
            false_positives++;
        }
    }
    free(buffer);
    /* About 1% expected; allow for the variance of block loads. */
    SX_CHECK(false_positives > probes / 400);
    SX_CHECK(false_positives < probes / 60);
}

static void test_bloom_view_rejects_bad_headers(void) {
    uint8_t buffer[96];
    sx_bloom bloom;
    SX_CHECK(sx_bloom_init(&bloom, buffer, sizeof(buffer)) == SX_OK);
    sx_bloom_view view;
    SX_CHECK(sx_bloom_view_open(&view, buffer, sizeof(buffer)) == SX_OK);
    SX_CHECK(sx_bloom_view_open(&view, buffer, sizeof(buffer) - 32) == SX_ERR_CORRUPT);
    SX_CHECK(sx_bloom_view_open(&view, buffer, 10) == SX_ERR_CORRUPT);
    buffer[4] = 2; /* unknown version */
    SX_CHECK(sx_bloom_view_open(&view, buffer, sizeof(buffer)) == SX_ERR_CORRUPT);
    buffer[4] = 1;
    buffer[0] = 0; /* zero blocks */
    SX_CHECK(sx_bloom_view_open(&view, buffer, sizeof(buffer)) == SX_ERR_CORRUPT);
}

/*
 * The layout is frozen: a known filter must keep its exact bytes. The expected hash of the
 * serialized filter comes from an independent model of the Parquet specification written
 * in Python with the reference xxhash package.
 */
static void test_bloom_layout_is_stable(void) {
    uint8_t buffer[96];
    sx_bloom bloom;
    SX_CHECK(sx_bloom_init(&bloom, buffer, sizeof(buffer)) == SX_OK);
    const char* keys[] = {"cpu,host=a", "cpu,host=b", "mem,host=a"};
    for (size_t i = 0; i < SX_ARRAY_SIZE(keys); i++) {
        sx_bloom_add(&bloom, sx_bloom_hash(keys[i], strlen(keys[i])));
    }
    /* Every key sets one bit in each word of one block; these three do not collide. */
    unsigned bits = 0;
    for (size_t i = SX_BLOOM_HEADER_SIZE; i < sizeof(buffer); i++) {
        bits += sx_popcount_u64(buffer[i]);
    }
    SX_CHECK_EQ_U64(bits, 24);
    SX_CHECK_EQ_U64(buffer[0], 2);
    SX_CHECK_EQ_U64(buffer[4], 1);
    SX_CHECK_EQ_U64(sx_xxh64(buffer, sizeof(buffer), 0), UINT64_C(0xa1d2b17cbfc10025));
}

static const sx_test_case bloom_cases[] = {
    {"xxh64_reference_vectors", test_xxh64_reference_vectors},
    {"xxh64_ignores_alignment", test_xxh64_ignores_alignment},
    {"bloom_sizes", test_bloom_sizes},
    {"bloom_membership_and_rate", test_bloom_membership_and_rate},
    {"bloom_view_rejects_bad_headers", test_bloom_view_rejects_bad_headers},
    {"bloom_layout_is_stable", test_bloom_layout_is_stable},
};

const sx_test_suite sx_test_suite_bloom = SX_TEST_SUITE("bloom", bloom_cases);
