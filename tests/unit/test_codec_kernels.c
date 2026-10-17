/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Every SIMD kernel set the machine supports must produce exactly the output of the
 * portable kernels (see encoding/kernels.h). The tests compare them on random inputs over
 * all widths and many counts, and run the decoders of every set on random and corrupted
 * payloads.
 */

#include "sx_test.h"

#include "common/bits.h"
#include "common/compiler.h"
#include "common/rng.h"
#include "encoding/bitpack.h"
#include "encoding/float_codec.h"
#include "encoding/int_codec.h"
#include "encoding/kernels.h"

#include <math.h>
#include <stdio.h>

#define MAX_PACKED (SX_VECTOR_SIZE * sizeof(uint64_t))

/* Counts around row, block, and word boundaries. */
static const uint32_t counts[] = {
    1,   2,   3,   7,   8,   9,   15,  16,  17,  31,  32,   33,   63,   64,   65,   100,  127,  128,
    129, 240, 241, 255, 256, 257, 500, 511, 512, 513, 1000, 1008, 1009, 1015, 1016, 1023, 1024,
};

/* The kernel table of `level`, leaving the active level unchanged. */
static const sx_codec_kernels* level_kernels(sx_simd_level level) {
    sx_simd_level previous = sx_codec_kernels_active()->level;
    sx_simd_set_level(level);
    const sx_codec_kernels* kernels = sx_codec_kernels_active();
    sx_simd_set_level(previous);
    return kernels;
}

/* Fills `levels` with the supported levels other than scalar; returns how many. */
static uint32_t simd_levels(sx_simd_level* levels) {
    uint32_t count = 0;
    for (unsigned level = SX_SIMD_SCALAR + 1u; level < SX_SIMD_LEVEL_COUNT; level++) {
        if (sx_simd_level_supported((sx_simd_level)level)) {
            levels[count] = (sx_simd_level)level;
            count++;
        }
    }
    return count;
}

static void fill_random(sx_rng* rng, void* data, size_t size) {
    uint8_t* bytes = data;
    for (size_t i = 0; i < size; i++) {
        bytes[i] = (uint8_t)sx_rng_next(rng);
    }
}

/* A multiplier for decode_for: each of the kernels' multiplication paths. */
static uint64_t pick_multiplier(sx_rng* rng, uint32_t round) {
    switch (round % 5u) {
        case 0:
            return 1;
        case 1:
            return 100;
        case 2:
            return UINT32_MAX;
        case 3:
            return (UINT64_C(1) << 32) + 3u;
        default:
            return sx_rng_next(rng);
    }
}

static void test_supported_levels(void) {
    SX_CHECK(sx_simd_level_supported(SX_SIMD_SCALAR));
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t count = simd_levels(levels);
    printf("    kernel sets compared with scalar:");
    for (uint32_t i = 0; i < count; i++) {
        printf(" %s", sx_simd_level_name(levels[i]));
    }
    printf("%s (active: %s)\n", count == 0 ? " none" : "",
           sx_simd_level_name(sx_codec_kernels_active()->level));
    for (unsigned level = 0; level < SX_SIMD_LEVEL_COUNT; level++) {
        sx_simd_level parsed;
        SX_CHECK(sx_simd_level_from_name(sx_simd_level_name((sx_simd_level)level), &parsed));
        SX_CHECK_EQ_U64(parsed, level);
    }
}

static void test_bitpack_matches_scalar(void) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    sx_rng rng;
    sx_rng_seed(&rng, 1);
    uint64_t values[SX_VECTOR_SIZE];
    uint8_t expected[MAX_PACKED];
    uint8_t actual[MAX_PACKED];
    for (uint32_t width = 0; width <= 64; width++) {
        for (size_t c = 0; c < SX_ARRAY_SIZE(counts); c++) {
            uint32_t count = counts[c];
            fill_random(&rng, values, count * sizeof(uint64_t));
            uint64_t base = width % 3u == 0 ? 0 : sx_rng_next(&rng);
            size_t size = sx_bitpack_size(count, width);
            scalar->bitpack(values, count, width, base, expected);
            for (uint32_t l = 0; l < level_count; l++) {
                fill_random(&rng, actual, size);
                level_kernels(levels[l])->bitpack(values, count, width, base, actual);
                if (memcmp(actual, expected, size) != 0) {
                    sx_test_fail(__FILE__, __LINE__, "%s bitpack differs: width %u count %u",
                                 sx_simd_level_name(levels[l]), width, count);
                    return;
                }
            }
        }
    }
}

static void test_unpack_and_decode_for_match_scalar(void) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    sx_rng rng;
    sx_rng_seed(&rng, 2);
    uint8_t packed[MAX_PACKED];
    uint64_t expected[SX_VECTOR_SIZE];
    uint64_t actual[SX_VECTOR_SIZE];
    uint32_t round = 0;
    for (uint32_t width = 0; width <= 64; width++) {
        for (size_t c = 0; c < SX_ARRAY_SIZE(counts); c++) {
            uint32_t count = counts[c];
            size_t padded = sx_bitpack_padded_count(count) * sizeof(uint64_t);
            fill_random(&rng, packed, sx_bitpack_size(count, width));
            uint64_t base = sx_rng_next(&rng);
            uint64_t multiplier = pick_multiplier(&rng, round);
            round++;
            for (uint32_t l = 0; l < level_count; l++) {
                const sx_codec_kernels* kernels = level_kernels(levels[l]);
                const char* name = sx_simd_level_name(levels[l]);
                scalar->bitunpack(packed, count, width, expected);
                kernels->bitunpack(packed, count, width, actual);
                if (memcmp(actual, expected, padded) != 0) {
                    sx_test_fail(__FILE__, __LINE__, "%s bitunpack differs: width %u count %u",
                                 name, width, count);
                    return;
                }
                scalar->decode_for(packed, count, width, base, multiplier, expected);
                kernels->decode_for(packed, count, width, base, multiplier, actual);
                if (memcmp(actual, expected, padded) != 0) {
                    sx_test_fail(__FILE__, __LINE__,
                                 "%s decode_for differs: width %u count %u multiplier %" PRIu64,
                                 name, width, count, multiplier);
                    return;
                }
            }
        }
    }
}

/* Scales of a random exponent/factor pair. */
static sx_alp_scales random_scales(sx_rng* rng) {
    static const double powers[19] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8, 1e9,
                                      1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};
    static const double inverse_powers[19] = {
        1e0,   1e-1,  1e-2,  1e-3,  1e-4,  1e-5,  1e-6,  1e-7,  1e-8,  1e-9,
        1e-10, 1e-11, 1e-12, 1e-13, 1e-14, 1e-15, 1e-16, 1e-17, 1e-18,
    };
    uint32_t exponent = (uint32_t)sx_rng_below(rng, 19);
    uint32_t factor = (uint32_t)sx_rng_below(rng, exponent + 1u);
    sx_alp_scales scales = {
        .encode_exponent = powers[exponent],
        .encode_factor = inverse_powers[factor],
        .decode_factor = powers[factor],
        .decode_exponent = inverse_powers[exponent],
    };
    return scales;
}

static void test_decode_alp_matches_scalar(void) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    sx_rng rng;
    sx_rng_seed(&rng, 3);
    uint8_t packed[MAX_PACKED];
    double expected[SX_VECTOR_SIZE];
    double actual[SX_VECTOR_SIZE];
    for (uint32_t width = 0; width <= 64; width++) {
        for (size_t c = 0; c < SX_ARRAY_SIZE(counts); c++) {
            uint32_t count = counts[c];
            fill_random(&rng, packed, sx_bitpack_size(count, width));
            /* Bases near zero give ordinary values; random ones give anything, NaN too. */
            uint64_t base = c % 2u == 0 ? sx_rng_below(&rng, 1u << 20) : sx_rng_next(&rng);
            sx_alp_scales scales = random_scales(&rng);
            scalar->decode_alp(packed, count, width, base, &scales, expected);
            for (uint32_t l = 0; l < level_count; l++) {
                level_kernels(levels[l])->decode_alp(packed, count, width, base, &scales, actual);
                if (memcmp(actual, expected, sx_bitpack_padded_count(count) * sizeof(double)) !=
                    0) {
                    sx_test_fail(__FILE__, __LINE__, "%s decode_alp differs: width %u count %u",
                                 sx_simd_level_name(levels[l]), width, count);
                    return;
                }
            }
        }
    }
}

static void test_rebuild_deltas_matches_scalar(void) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    sx_rng rng;
    sx_rng_seed(&rng, 4);
    uint64_t deltas[SX_VECTOR_SIZE];
    uint64_t expected[SX_VECTOR_SIZE + 1];
    uint64_t actual[SX_VECTOR_SIZE + 1];
    for (uint32_t slot_count = 1; slot_count < SX_VECTOR_SIZE; slot_count++) {
        /* Padding slots hold garbage too: it must never reach the output. */
        fill_random(&rng, deltas, sizeof(deltas));
        uint64_t first = sx_rng_next(&rng);
        scalar->rebuild_deltas(deltas, slot_count, first, expected);
        for (uint32_t l = 0; l < level_count; l++) {
            /* Values past the end must be left alone. */
            memset(actual, 0xA5, sizeof(actual));
            level_kernels(levels[l])->rebuild_deltas(deltas, slot_count, first, actual);
            if (memcmp(actual, expected, (slot_count + 1u) * sizeof(uint64_t)) != 0) {
                sx_test_fail(__FILE__, __LINE__, "%s rebuild_deltas differs: %u slots",
                             sx_simd_level_name(levels[l]), slot_count);
                return;
            }
            for (uint32_t i = slot_count + 1u; i < SX_VECTOR_SIZE + 1u; i++) {
                if (actual[i] != UINT64_C(0xA5A5A5A5A5A5A5A5)) {
                    sx_test_fail(__FILE__, __LINE__, "%s rebuild_deltas wrote value %u of %u",
                                 sx_simd_level_name(levels[l]), i, slot_count + 1u);
                    return;
                }
            }
        }
    }
}

/* Doubles of one kind: decimals with a few digits, special values, or random bits. */
static double random_double(sx_rng* rng, uint32_t kind) {
    switch (kind % 4u) {
        case 0:
            return (double)((int64_t)sx_rng_below(rng, 2000000) - 1000000) / 100.0;
        case 1: {
            static const double specials[] = {
                0.0,
                -0.0,
                INFINITY,
                -INFINITY,
                NAN,
                2251799813685248.0,
                -2251799813685248.0,
                2251799813685247.0,
                4.9e-324,
                1e300,
                0.1,
                123456.789,
            };
            return specials[sx_rng_below(rng, SX_ARRAY_SIZE(specials))];
        }
        case 2:
            return sx_bits_to_double(sx_rng_next(rng));
        default:
            return (double)(int64_t)sx_rng_next(rng) * 1e-9;
    }
}

static void test_encode_alp_matches_scalar(void) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    sx_rng rng;
    sx_rng_seed(&rng, 5);
    double values[SX_VECTOR_SIZE];
    int64_t expected_digits[SX_VECTOR_SIZE];
    int64_t actual_digits[SX_VECTOR_SIZE];
    uint64_t expected_bits[SX_VECTOR_SIZE / 64];
    uint64_t actual_bits[SX_VECTOR_SIZE / 64];
    for (uint32_t round = 0; round < 400; round++) {
        uint32_t count = counts[round % SX_ARRAY_SIZE(counts)];
        /* Mostly one kind of value, with some values of other kinds mixed in. */
        for (uint32_t i = 0; i < count; i++) {
            uint32_t kind = sx_rng_below(&rng, 8) == 0 ? (uint32_t)sx_rng_next(&rng) : round;
            values[i] = random_double(&rng, kind);
        }
        sx_alp_scales scales = random_scales(&rng);
        sx_alp_digit_range expected_range;
        uint32_t expected = scalar->encode_alp(values, count, &scales, expected_digits,
                                               expected_bits, &expected_range);
        for (uint32_t l = 0; l < level_count; l++) {
            const char* name = sx_simd_level_name(levels[l]);
            sx_alp_digit_range range;
            uint32_t actual = level_kernels(levels[l])->encode_alp(
                values, count, &scales, actual_digits, actual_bits, &range);
            SX_CHECK_EQ_U64(actual, expected);
            SX_CHECK_EQ_MEM(actual_bits, expected_bits, sx_div_round_up(count, 64) * 8u);
            SX_CHECK_EQ_I64(range.min, expected_range.min);
            SX_CHECK_EQ_I64(range.max, expected_range.max);
            for (uint32_t i = 0; i < count; i++) {
                bool exception = (expected_bits[i / 64u] >> (i % 64u)) & 1u;
                if (!exception && actual_digits[i] != expected_digits[i]) {
                    sx_test_fail(__FILE__, __LINE__, "%s encode_alp digits %u differ", name, i);
                    return;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------------------------------- */
/* Whole decoders on random and corrupted payloads                                          */
/* ---------------------------------------------------------------------------------------- */

typedef struct decode_result {
    sx_err err;
    uint64_t values[SX_VECTOR_SIZE]; /* int64 values or double bits */
} decode_result;

static void decode_with(sx_simd_level level, bool doubles, sx_encoding encoding,
                        const uint8_t* payload, size_t size, uint32_t count,
                        decode_result* result) {
    sx_simd_level previous = sx_codec_kernels_active()->level;
    sx_simd_set_level(level);
    memset(result->values, 0, sizeof(result->values));
    if (doubles) {
        double out[SX_VECTOR_SIZE];
        result->err = sx_double_decode(encoding, payload, size, count, out);
        memcpy(result->values, out, count * sizeof(double));
    } else {
        int64_t out[SX_VECTOR_SIZE];
        result->err = sx_int64_decode(encoding, payload, size, count, out);
        memcpy(result->values, out, count * sizeof(int64_t));
    }
    sx_simd_set_level(previous);
}

/* Decodes with every level and checks that all agree with scalar. */
static bool decoders_agree(bool doubles, sx_encoding encoding, const uint8_t* payload, size_t size,
                           uint32_t count) {
    static decode_result expected;
    static decode_result actual;
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    decode_with(SX_SIMD_SCALAR, doubles, encoding, payload, size, count, &expected);
    for (uint32_t l = 0; l < level_count; l++) {
        decode_with(levels[l], doubles, encoding, payload, size, count, &actual);
        bool same = actual.err == expected.err &&
                    (expected.err != SX_OK ||
                     memcmp(actual.values, expected.values, count * sizeof(uint64_t)) == 0);
        if (!same) {
            sx_test_fail(__FILE__, __LINE__, "%s %s decoder differs on %zu bytes, count %u",
                         sx_simd_level_name(levels[l]), sx_encoding_name(encoding), size, count);
            return false;
        }
    }
    return true;
}

static void test_decoders_agree_on_random_payloads(void) {
    static const sx_encoding int_encodings[] = {
        SX_ENCODING_FOR_BITPACK,
        SX_ENCODING_DELTA_FOR,
        SX_ENCODING_DELTA_PATCHED,
    };
    static const sx_encoding double_encodings[] = {SX_ENCODING_ALP, SX_ENCODING_ALP_RD};
    sx_rng rng;
    sx_rng_seed(&rng, 6);
    static uint8_t payload[MAX_PACKED + 256];
    for (uint32_t round = 0; round < 3000; round++) {
        uint32_t count = counts[round % SX_ARRAY_SIZE(counts)];
        size_t size = sx_rng_below(&rng, sizeof(payload));
        fill_random(&rng, payload, size);
        /* Plausible headers reach the kernels more often than random ones. */
        payload[16] = (uint8_t)sx_rng_below(&rng, 65);
        payload[24] = (uint8_t)sx_rng_below(&rng, 65);
        payload[0] = (uint8_t)sx_rng_below(&rng, 19);
        payload[1] = (uint8_t)sx_rng_below(&rng, (uint64_t)payload[0] + 1u);
        sx_encoding encoding = int_encodings[round % SX_ARRAY_SIZE(int_encodings)];
        SX_CHECK(decoders_agree(false, encoding, payload, size, count));
        encoding = double_encodings[round % SX_ARRAY_SIZE(double_encodings)];
        SX_CHECK(decoders_agree(true, encoding, payload, size, count));
    }
}

static void test_decoders_agree_on_corrupted_chunks(void) {
    sx_rng rng;
    sx_rng_seed(&rng, 7);
    static uint8_t payload[MAX_PACKED];
    int64_t ints[SX_VECTOR_SIZE];
    double doubles[SX_VECTOR_SIZE];
    for (uint32_t round = 0; round < 400; round++) {
        uint32_t count = counts[round % SX_ARRAY_SIZE(counts)];
        /* A gapped random walk: FOR, DELTA_FOR, or DELTA_PATCHED depending on the round. */
        int64_t value = (int64_t)sx_rng_next(&rng) >> 8;
        for (uint32_t i = 0; i < count; i++) {
            value += (int64_t)sx_rng_below(&rng, 1u << (round % 20u)) * 100;
            if (sx_rng_below(&rng, 64) == 0) {
                value += INT64_C(1) << 40;
            }
            ints[i] = value;
            doubles[i] = (double)(value % 1000000) / 100.0;
        }
        sx_byte_writer writer;
        sx_encoding encoding;
        sx_writer_init(&writer, payload, sizeof(payload));
        SX_CHECK(sx_int64_encode(ints, count, &writer, &encoding) == SX_OK);
        size_t size = writer.size;
        for (uint32_t flip = 0; flip < 4; flip++) {
            if (size > 0) {
                payload[sx_rng_below(&rng, size)] ^= (uint8_t)(1u << sx_rng_below(&rng, 8));
            }
            SX_CHECK(decoders_agree(false, encoding, payload, size, count));
        }
        sx_writer_init(&writer, payload, sizeof(payload));
        SX_CHECK(sx_double_encode(doubles, count, &writer, &encoding) == SX_OK);
        size = writer.size;
        for (uint32_t flip = 0; flip < 4; flip++) {
            if (size > 0) {
                payload[sx_rng_below(&rng, size)] ^= (uint8_t)(1u << sx_rng_below(&rng, 8));
            }
            SX_CHECK(decoders_agree(true, encoding, payload, size, count));
        }
    }
}

static const sx_test_case cases[] = {
    {"supported_levels", test_supported_levels},
    {"bitpack_matches_scalar", test_bitpack_matches_scalar},
    {"unpack_and_decode_for_match_scalar", test_unpack_and_decode_for_match_scalar},
    {"decode_alp_matches_scalar", test_decode_alp_matches_scalar},
    {"rebuild_deltas_matches_scalar", test_rebuild_deltas_matches_scalar},
    {"encode_alp_matches_scalar", test_encode_alp_matches_scalar},
    {"decoders_agree_on_random_payloads", test_decoders_agree_on_random_payloads},
    {"decoders_agree_on_corrupted_chunks", test_decoders_agree_on_corrupted_chunks},
};

const sx_test_suite sx_test_suite_codec_kernels = SX_TEST_SUITE("codec_kernels", cases);
