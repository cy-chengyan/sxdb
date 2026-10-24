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

#include <inttypes.h>
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

/* Values of `scratch` that decode_deltas writes: rows 0 to the last one rebuild_deltas reads. */
static size_t delta_scratch_values(uint32_t slot_count) {
    uint32_t rows = sx_bitpack_rows(slot_count);
    uint32_t last_row = sx_delta_scratch_last_row(rows);
    return (size_t)((last_row > rows ? last_row : rows) + 1u) * SX_BITPACK_LANES;
}

/* The portable decode_deltas against its contract: decode_for rows behind a zero row. */
static bool check_decode_deltas_layout(const uint64_t* scratch, const uint64_t* lane_totals,
                                       const uint8_t* packed, uint32_t slot_count, uint32_t width,
                                       uint64_t base, uint64_t multiplier) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    uint64_t deltas[SX_VECTOR_SIZE];
    scalar->decode_for(packed, slot_count, width, base, multiplier, deltas);
    uint32_t rows = sx_bitpack_rows(slot_count);
    uint64_t totals[SX_BITPACK_LANES] = {0};
    for (size_t i = 0; i < delta_scratch_values(slot_count); i++) {
        size_t row = i / SX_BITPACK_LANES;
        bool delta_row = row >= 1u && row <= rows;
        uint64_t expected = delta_row ? deltas[i - SX_BITPACK_LANES] : 0;
        if (scratch[i] != expected) {
            sx_test_fail(__FILE__, __LINE__, "scalar decode_deltas value %zu wrong: %u slots", i,
                         slot_count);
            return false;
        }
        totals[i % SX_BITPACK_LANES] += expected;
    }
    if (memcmp(totals, lane_totals, sizeof(totals)) != 0) {
        sx_test_fail(__FILE__, __LINE__, "scalar decode_deltas totals wrong: %u slots", slot_count);
        return false;
    }
    return true;
}

static void test_decode_deltas_matches_scalar(void) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    sx_rng rng;
    sx_rng_seed(&rng, 8);
    uint8_t packed[MAX_PACKED];
    uint64_t expected[SX_DELTA_SCRATCH_SIZE];
    uint64_t actual[SX_DELTA_SCRATCH_SIZE];
    uint64_t expected_totals[SX_BITPACK_LANES];
    uint64_t actual_totals[SX_BITPACK_LANES];
    uint32_t round = 0;
    for (uint32_t width = 0; width <= 64; width++) {
        for (size_t c = 0; c < SX_ARRAY_SIZE(counts); c++) {
            uint32_t slot_count = counts[c] < SX_VECTOR_SIZE ? counts[c] : SX_VECTOR_SIZE - 1u;
            fill_random(&rng, packed, sx_bitpack_size(slot_count, width));
            uint64_t base = sx_rng_next(&rng);
            uint64_t multiplier = pick_multiplier(&rng, round);
            round++;
            /* Different garbage before each call: every value written is compared. */
            memset(expected, 0x5A, sizeof(expected));
            scalar->decode_deltas(packed, slot_count, width, base, multiplier, expected,
                                  expected_totals);
            if (!check_decode_deltas_layout(expected, expected_totals, packed, slot_count, width,
                                            base, multiplier)) {
                return;
            }
            size_t size = delta_scratch_values(slot_count) * sizeof(uint64_t);
            for (uint32_t l = 0; l < level_count; l++) {
                memset(actual, 0xA5, sizeof(actual));
                level_kernels(levels[l])->decode_deltas(packed, slot_count, width, base, multiplier,
                                                        actual, actual_totals);
                if (memcmp(actual, expected, size) != 0 ||
                    memcmp(actual_totals, expected_totals, sizeof(expected_totals)) != 0) {
                    sx_test_fail(
                        __FILE__, __LINE__,
                        "%s decode_deltas differs: width %u, %u slots, multiplier %" PRIu64,
                        sx_simd_level_name(levels[l]), width, slot_count, multiplier);
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
    static uint64_t scratch[SX_DELTA_SCRATCH_SIZE];
    uint64_t lane_totals[SX_BITPACK_LANES];
    uint64_t expected[SX_VECTOR_SIZE + 16];
    uint64_t actual[SX_VECTOR_SIZE + 16];
    for (uint32_t slot_count = 1; slot_count < SX_VECTOR_SIZE; slot_count++) {
        uint32_t rows = sx_bitpack_rows(slot_count);
        /* Row 0 is zero, as decode_deltas leaves it; the padding and the rows after the last
         * hold garbage, which must not reach the first slot_count + 1 values. */
        fill_random(&rng, scratch, sizeof(scratch));
        memset(scratch, 0, SX_BITPACK_ROW_BYTES);
        memset(lane_totals, 0, sizeof(lane_totals));
        for (size_t i = SX_BITPACK_LANES; i < (size_t)(rows + 1u) * SX_BITPACK_LANES; i++) {
            lane_totals[i % SX_BITPACK_LANES] += scratch[i];
        }
        uint64_t first = sx_rng_next(&rng);
        size_t written =
            slot_count + 1u > SX_BITPACK_LANES * rows ? slot_count + 1u : SX_BITPACK_LANES * rows;
        memset(expected, 0x5A, sizeof(expected));
        scalar->rebuild_deltas(scratch, slot_count, first, lane_totals, expected);

        /* out[0] = first and out[i + 1] = out[i] + delta of slot i. */
        uint64_t value = first;
        SX_CHECK_EQ_U64(expected[0], first);
        for (uint32_t i = 0; i < slot_count; i++) {
            value += scratch[SX_BITPACK_LANES + sx_bitpack_lane_position(i, rows)];
            if (expected[i + 1u] != value) {
                sx_test_fail(__FILE__, __LINE__, "scalar rebuild_deltas value %u wrong: %u slots",
                             i + 1u, slot_count);
                return;
            }
        }
        SX_CHECK(written <= SX_VECTOR_SIZE);
        for (size_t i = written; i < SX_ARRAY_SIZE(expected); i++) {
            SX_CHECK_EQ_U64(expected[i], UINT64_C(0x5A5A5A5A5A5A5A5A));
        }

        for (uint32_t l = 0; l < level_count; l++) {
            memset(actual, 0xA5, sizeof(actual));
            level_kernels(levels[l])->rebuild_deltas(scratch, slot_count, first, lane_totals,
                                                     actual);
            if (memcmp(actual, expected, written * sizeof(uint64_t)) != 0) {
                sx_test_fail(__FILE__, __LINE__, "%s rebuild_deltas differs: %u slots",
                             sx_simd_level_name(levels[l]), slot_count);
                return;
            }
            for (size_t i = written; i < SX_ARRAY_SIZE(actual); i++) {
                if (actual[i] != UINT64_C(0xA5A5A5A5A5A5A5A5)) {
                    sx_test_fail(__FILE__, __LINE__, "%s rebuild_deltas wrote value %zu of %zu",
                                 sx_simd_level_name(levels[l]), i, written);
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
/* Integer encoding kernels                                                                 */
/* ---------------------------------------------------------------------------------------- */

#define SENTINEL UINT64_C(0xA5A5A5A5A5A5A5A5)

/* Integers of one kind: a walk, long runs, extremes whose deltas wrap, regular steps with
 * a few large gaps, or random bits. */
static void fill_integers(sx_rng* rng, uint32_t kind, int64_t* values, uint32_t count) {
    uint64_t value = sx_rng_next(rng);
    for (uint32_t i = 0; i < count; i++) {
        switch (kind % 5u) {
            case 0:
                value += sx_rng_below(rng, 7) - 3u;
                break;
            case 1:
                if (sx_rng_below(rng, 50) == 0) {
                    value = sx_rng_below(rng, 4);
                }
                break;
            case 2:
                value = sx_rng_below(rng, 2) == 0 ? (uint64_t)INT64_MIN + sx_rng_below(rng, 3)
                                                  : (uint64_t)INT64_MAX - sx_rng_below(rng, 3);
                break;
            case 3:
                value += sx_rng_below(rng, 64) == 0 ? sx_rng_next(rng) >> 8 : 60u;
                break;
            default:
                value = sx_rng_next(rng);
                break;
        }
        values[i] = (int64_t)value;
    }
}

static void test_summarize_ints_matches_scalar(void) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    sx_rng rng;
    sx_rng_seed(&rng, 6);
    int64_t values[SX_VECTOR_SIZE];
    uint64_t expected_deltas[SX_VECTOR_SIZE];
    uint64_t actual_deltas[SX_VECTOR_SIZE];
    for (uint32_t kind = 0; kind < 10u; kind++) {
        for (size_t c = 0; c < SX_ARRAY_SIZE(counts); c++) {
            uint32_t count = counts[c];
            fill_integers(&rng, kind, values, count);
            sx_int_summary expected;
            scalar->summarize_ints(values, count, expected_deltas, &expected);
            for (uint32_t l = 0; l < level_count; l++) {
                const char* name = sx_simd_level_name(levels[l]);
                sx_int_summary actual;
                memset(actual_deltas, 0xA5, sizeof(actual_deltas));
                level_kernels(levels[l])->summarize_ints(values, count, actual_deltas, &actual);
                SX_CHECK_EQ_I64(actual.min, expected.min);
                SX_CHECK_EQ_I64(actual.max, expected.max);
                SX_CHECK_EQ_I64(actual.delta_min, expected.delta_min);
                SX_CHECK_EQ_I64(actual.delta_max, expected.delta_max);
                SX_CHECK_EQ_U64(actual.changes, expected.changes);
                SX_CHECK_EQ_MEM(actual_deltas, expected_deltas, (count - 1u) * sizeof(uint64_t));
                for (uint32_t i = count - 1u; i < SX_VECTOR_SIZE; i++) {
                    if (actual_deltas[i] != SENTINEL) {
                        sx_test_fail(__FILE__, __LINE__, "%s summarize_ints wrote delta %u of %u",
                                     name, i, count - 1u);
                        return;
                    }
                }
            }
        }
    }
}

/* A divisor for the multiple tests: each special case of the test, or random. */
static uint64_t pick_divisor(sx_rng* rng, uint32_t round) {
    static const uint64_t divisors[] = {
        0, 1, 2, 3, 8, 100, 60000000000u, UINT64_C(1) << 63, UINT64_MAX,
    };
    if (round % 4u == 3u) {
        return sx_rng_next(rng) >> sx_rng_below(rng, 64);
    }
    return divisors[sx_rng_below(rng, SX_ARRAY_SIZE(divisors))];
}

/* base + k * divisor (wrapping) with small or large k, so that most numbers minus the base
 * are multiples; then a few numbers are moved off by 1 to divisor - 1. */
static void fill_multiples(sx_rng* rng, uint64_t base, uint64_t divisor, uint64_t* numbers,
                           uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint64_t k = sx_rng_below(rng, 2) == 0 ? sx_rng_below(rng, 1000) : sx_rng_next(rng);
        numbers[i] = base + k * divisor;
    }
    uint32_t misses = (uint32_t)sx_rng_below(rng, 3);
    for (uint32_t m = 0; m < misses && divisor != 1; m++) {
        uint64_t off = divisor == 0 ? sx_rng_next(rng) | 1u : 1u + sx_rng_below(rng, divisor - 1u);
        numbers[sx_rng_below(rng, count)] += off;
    }
}

static void test_first_non_multiple_matches_scalar(void) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    sx_rng rng;
    sx_rng_seed(&rng, 7);
    uint64_t numbers[SX_VECTOR_SIZE];
    uint64_t expected_quotients[SX_VECTOR_SIZE];
    uint64_t actual_quotients[SX_VECTOR_SIZE];
    for (uint32_t round = 0; round < 2000; round++) {
        uint32_t count = counts[round % SX_ARRAY_SIZE(counts)];
        uint64_t divisor = pick_divisor(&rng, round);
        uint64_t base = round % 3u == 0 ? 0 : sx_rng_next(&rng);
        fill_multiples(&rng, base, divisor, numbers, count);
        sx_multiple_test test = sx_multiple_test_make(divisor);
        uint32_t starts[] = {0, (uint32_t)sx_rng_below(&rng, count), count};
        for (size_t s = 0; s < SX_ARRAY_SIZE(starts); s++) {
            uint32_t start = starts[s];
            uint32_t expected =
                scalar->first_non_multiple(numbers, start, count, base, &test, expected_quotients);
            for (uint32_t i = start; i < expected; i++) {
                SX_CHECK_EQ_U64(expected_quotients[i] * divisor, numbers[i] - base);
            }
            for (uint32_t l = 0; l < level_count; l++) {
                const char* name = sx_simd_level_name(levels[l]);
                uint32_t actual = level_kernels(levels[l])->first_non_multiple(
                    numbers, start, count, base, &test, actual_quotients);
                if (actual != expected) {
                    sx_test_fail(__FILE__, __LINE__,
                                 "%s first_non_multiple: %u, expected %u (divisor %" PRIu64
                                 ", count %u, start %u)",
                                 name, actual, expected, divisor, count, start);
                    return;
                }
                SX_CHECK_EQ_MEM(actual_quotients + start, expected_quotients + start,
                                (expected - start) * sizeof(uint64_t));
            }
        }
    }
}

static void test_divide_exact_matches_scalar(void) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    sx_rng rng;
    sx_rng_seed(&rng, 8);
    uint64_t numbers[SX_VECTOR_SIZE];
    uint64_t expected[SX_VECTOR_SIZE];
    uint64_t actual[SX_VECTOR_SIZE];
    for (uint32_t round = 0; round < 1000; round++) {
        uint32_t count = counts[round % SX_ARRAY_SIZE(counts)];
        uint64_t divisor = pick_divisor(&rng, round);
        uint64_t base = round % 3u == 0 ? 0 : sx_rng_next(&rng);
        fill_multiples(&rng, base, divisor, numbers, count);
        sx_exact_divisor exact = sx_exact_divisor_make(divisor);
        scalar->divide_exact(numbers, count, base, &exact, expected);
        for (uint32_t l = 0; l < level_count; l++) {
            const sx_codec_kernels* kernels = level_kernels(levels[l]);
            memset(actual, 0xA5, sizeof(actual));
            kernels->divide_exact(numbers, count, base, &exact, actual);
            SX_CHECK_EQ_MEM(actual, expected, count * sizeof(uint64_t));
            for (uint32_t i = count; i < SX_VECTOR_SIZE; i++) {
                SX_CHECK_EQ_U64(actual[i], SENTINEL);
            }
            /* In place. */
            memcpy(actual, numbers, count * sizeof(uint64_t));
            kernels->divide_exact(actual, count, base, &exact, actual);
            SX_CHECK_EQ_MEM(actual, expected, count * sizeof(uint64_t));
        }
    }
}

static void test_width_counts_match_scalar(void) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    sx_rng rng;
    sx_rng_seed(&rng, 9);
    uint64_t numbers[SX_VECTOR_SIZE];
    uint32_t expected[65];
    uint32_t actual[65];
    for (uint32_t max_width = 0; max_width <= 64; max_width++) {
        for (size_t c = 0; c < SX_ARRAY_SIZE(counts); c++) {
            uint32_t count = counts[c];
            /* A few common widths, sometimes up to the maximum. */
            uint32_t common = (uint32_t)sx_rng_below(&rng, max_width + 1u);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t width = sx_rng_below(&rng, 4) == 0
                                     ? (uint32_t)sx_rng_below(&rng, max_width + 1u)
                                     : common;
                numbers[i] =
                    width == 0 ? 0 : (sx_rng_next(&rng) | UINT64_C(1) << 63) >> (64u - width);
            }
            /* Stopping early, late, or never. */
            uint32_t enough = (uint32_t)sx_rng_below(&rng, count + 2u);
            uint32_t expected_lowest =
                scalar->width_counts(numbers, count, max_width, enough, expected);
            for (uint32_t l = 0; l < level_count; l++) {
                memset(actual, 0xA5, sizeof(actual));
                uint32_t lowest = level_kernels(levels[l])->width_counts(numbers, count, max_width,
                                                                         enough, actual);
                if (lowest != expected_lowest ||
                    memcmp(actual + lowest, expected + lowest,
                           (max_width + 1u - lowest) * sizeof(uint32_t)) != 0) {
                    sx_test_fail(__FILE__, __LINE__,
                                 "%s width_counts differ: max %u count %u enough %u",
                                 sx_simd_level_name(levels[l]), max_width, count, enough);
                    return;
                }
                for (uint32_t w = max_width + 1u; w <= 64; w++) {
                    SX_CHECK_EQ_U64(actual[w], UINT32_C(0xA5A5A5A5));
                }
            }
        }
    }
}

static void test_find_exceptions_matches_scalar(void) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    sx_rng rng;
    sx_rng_seed(&rng, 10);
    uint64_t offsets[SX_VECTOR_SIZE];
    uint64_t expected_bits[SX_VECTOR_SIZE / 64];
    uint64_t actual_bits[SX_VECTOR_SIZE / 64];
    for (uint32_t width = 0; width <= 64; width++) {
        for (size_t c = 0; c < SX_ARRAY_SIZE(counts); c++) {
            uint32_t count = counts[c];
            uint64_t limit = sx_low_mask_u64(width);
            /* Offsets within the limit, with exceptions from rare to frequent. */
            uint64_t rarity = 1u + sx_rng_below(&rng, 100);
            for (uint32_t i = 0; i < count; i++) {
                offsets[i] =
                    sx_rng_below(&rng, rarity) == 0 ? sx_rng_next(&rng) : sx_rng_next(&rng) & limit;
            }
            size_t words = sx_div_round_up(count, 64);
            uint32_t expected = scalar->find_exceptions(offsets, count, limit, expected_bits);
            uint32_t ones = 0;
            for (uint32_t i = 0; i < count; i++) {
                bool bit = (expected_bits[i / 64u] >> (i % 64u)) & 1u;
                SX_CHECK(bit == (offsets[i] > limit));
                ones += (uint32_t)bit;
            }
            SX_CHECK_EQ_U64(expected, ones);
            for (uint32_t l = 0; l < level_count; l++) {
                memset(actual_bits, 0xA5, sizeof(actual_bits));
                uint32_t actual =
                    level_kernels(levels[l])->find_exceptions(offsets, count, limit, actual_bits);
                SX_CHECK_EQ_U64(actual, expected);
                SX_CHECK_EQ_MEM(actual_bits, expected_bits, words * sizeof(uint64_t));
            }
        }
    }
}

static void test_to_lane_order_matches_scalar(void) {
    const sx_codec_kernels* scalar = level_kernels(SX_SIMD_SCALAR);
    sx_simd_level levels[SX_SIMD_LEVEL_COUNT];
    uint32_t level_count = simd_levels(levels);
    sx_rng rng;
    sx_rng_seed(&rng, 11);
    uint64_t values[SX_VECTOR_SIZE];
    uint64_t expected[SX_VECTOR_SIZE];
    uint64_t actual[SX_VECTOR_SIZE];
    for (uint32_t count = 1; count <= SX_VECTOR_SIZE; count++) {
        /* Values of up to 20 bits, some of them above the limit. */
        for (uint32_t i = 0; i < count; i++) {
            values[i] = sx_rng_next(&rng) >> (44u + sx_rng_below(&rng, 4));
        }
        uint64_t limit = count % 3u == 0 ? UINT64_MAX : sx_low_mask_u64(count % 21u);
        size_t size = sx_bitpack_padded_count(count) * sizeof(uint64_t);
        scalar->to_lane_order(values, count, limit, expected);
        for (uint32_t l = 0; l < level_count; l++) {
            fill_random(&rng, actual, sizeof(actual));
            level_kernels(levels[l])->to_lane_order(values, count, limit, actual);
            if (memcmp(actual, expected, size) != 0) {
                sx_test_fail(__FILE__, __LINE__, "%s to_lane_order differs: count %u",
                             sx_simd_level_name(levels[l]), count);
                return;
            }
        }
    }
}

/* The shared exact division: inverses, quotients, and the multiple test. */
static void test_exact_division(void) {
    sx_rng rng;
    sx_rng_seed(&rng, 14);
    for (uint32_t round = 0; round < 20000; round++) {
        uint64_t divisor = pick_divisor(&rng, round);
        if (divisor == 0) {
            divisor = 1u + sx_rng_below(&rng, 1000);
        }
        sx_exact_divisor exact = sx_exact_divisor_make(divisor);
        SX_CHECK_EQ_U64((divisor >> exact.shift) * exact.inverse, 1);
        uint64_t max_quotient = UINT64_MAX / divisor;
        uint64_t quotient = sx_rng_next(&rng);
        if (max_quotient != UINT64_MAX) {
            quotient %= max_quotient + 1u;
        }
        SX_CHECK_EQ_U64(sx_divide_exact(quotient * divisor, &exact), quotient);
        sx_multiple_test test = sx_multiple_test_make(divisor);
        uint64_t x = sx_rng_below(&rng, 2) == 0 ? quotient * divisor : sx_rng_next(&rng);
        SX_CHECK((sx_multiple_miss(x, &test) == 0) == (x % divisor == 0));
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
    {"decode_deltas_matches_scalar", test_decode_deltas_matches_scalar},
    {"rebuild_deltas_matches_scalar", test_rebuild_deltas_matches_scalar},
    {"encode_alp_matches_scalar", test_encode_alp_matches_scalar},
    {"summarize_ints_matches_scalar", test_summarize_ints_matches_scalar},
    {"first_non_multiple_matches_scalar", test_first_non_multiple_matches_scalar},
    {"divide_exact_matches_scalar", test_divide_exact_matches_scalar},
    {"width_counts_match_scalar", test_width_counts_match_scalar},
    {"find_exceptions_matches_scalar", test_find_exceptions_matches_scalar},
    {"to_lane_order_matches_scalar", test_to_lane_order_matches_scalar},
    {"exact_division", test_exact_division},
    {"decoders_agree_on_random_payloads", test_decoders_agree_on_random_payloads},
    {"decoders_agree_on_corrupted_chunks", test_decoders_agree_on_corrupted_chunks},
};

const sx_test_suite sx_test_suite_codec_kernels = SX_TEST_SUITE("codec_kernels", cases);
