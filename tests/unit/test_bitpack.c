/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sx_test.h"

#include "common/bits.h"
#include "common/compiler.h"
#include "common/rng.h"
#include "encoding/bitpack.h"

static void test_packed_sizes(void) {
    SX_CHECK_EQ_U64(sx_bitpack_size(1024, 0), 0);
    SX_CHECK_EQ_U64(sx_bitpack_size(1024, 1), 128);
    SX_CHECK_EQ_U64(sx_bitpack_size(1024, 7), 7 * 128);
    SX_CHECK_EQ_U64(sx_bitpack_size(1024, 64), 8192);
    SX_CHECK_EQ_U64(sx_bitpack_size(1, 1), 128);
    SX_CHECK_EQ_U64(sx_bitpack_size(17, 33), 256);
    SX_CHECK_EQ_U64(sx_bitpack_padded_count(1), 16);
    SX_CHECK_EQ_U64(sx_bitpack_padded_count(1024), 1024);
}

static void test_round_trip_every_width_and_count(void) {
    static const uint32_t counts[] = {1, 2, 15, 16, 17, 100, 1000, 1023, 1024};
    uint64_t values[SX_VECTOR_SIZE];
    uint64_t decoded[SX_VECTOR_SIZE];
    uint8_t packed[SX_VECTOR_SIZE * sizeof(uint64_t)];
    sx_rng rng;
    sx_rng_seed(&rng, 7);

    for (uint32_t width = 0; width <= 64; width++) {
        uint64_t mask = sx_low_mask_u64(width);
        for (size_t c = 0; c < SX_ARRAY_SIZE(counts); c++) {
            uint32_t count = counts[c];
            for (uint32_t i = 0; i < count; i++) {
                values[i] = sx_rng_next(&rng) & mask;
            }
            /* Make sure the largest value of the width is exercised. */
            values[count - 1] = mask;

            sx_bitpack_u64(values, count, width, packed);
            memset(decoded, 0xFF, sizeof(decoded));
            sx_bitunpack_u64(packed, count, width, decoded);

            for (uint32_t i = 0; i < count; i++) {
                if (decoded[i] != values[i]) {
                    sx_test_fail(__FILE__, __LINE__, "width %u count %u index %u differs", width,
                                 count, i);
                    return;
                }
            }
            for (uint32_t i = count; i < sx_bitpack_padded_count(count); i++) {
                SX_CHECK_EQ_U64(decoded[i], 0);
            }
        }
    }
}

static void test_values_are_masked_to_width(void) {
    uint64_t values[16];
    uint64_t decoded[16];
    uint8_t packed[16 * sizeof(uint64_t)];
    for (uint32_t i = 0; i < 16; i++) {
        values[i] = UINT64_MAX;
    }
    sx_bitpack_u64(values, 16, 3, packed);
    sx_bitunpack_u64(packed, 16, 3, decoded);
    for (uint32_t i = 0; i < 16; i++) {
        SX_CHECK_EQ_U64(decoded[i], 7);
    }
}

static const sx_test_case cases[] = {
    {"packed_sizes", test_packed_sizes},
    {"round_trip_widths_counts", test_round_trip_every_width_and_count},
    {"values_masked_to_width", test_values_are_masked_to_width},
};

const sx_test_suite sx_test_suite_bitpack = SX_TEST_SUITE("bitpack", cases);
