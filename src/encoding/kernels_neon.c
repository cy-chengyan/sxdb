/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * NEON codec kernels (ARM64). A row of 16 values is eight 128-bit vectors of two lanes.
 * Every function computes exactly what its portable counterpart in kernels_scalar.c
 * computes; see kernels.h for the contracts.
 */

#include "encoding/bitpack.h"
#include "encoding/kernels.h"
#include "encoding/kernels_alp.h"

#include "common/bits.h"
#include "common/compiler.h"

#if defined(SX_ARCH_ARM64)

#include <arm_neon.h>

#define VECTORS_PER_ROW 8u
#define VECTOR_LANES    ((size_t)2)

static inline int64_t min_i64(int64_t a, int64_t b) {
    return a < b ? a : b;
}

static inline int64_t max_i64(int64_t a, int64_t b) {
    return a > b ? a : b;
}

/* Packed blocks have no alignment guarantee, so they are accessed as bytes. */
static inline uint64x2_t load_bytes(const uint8_t* p) {
    return vreinterpretq_u64_u8(vld1q_u8(p));
}

static inline void store_bytes(uint8_t* p, uint64x2_t v) {
    vst1q_u8(p, vreinterpretq_u8_u64(v));
}

/* Unpacks row `row` of a block packed at `width` > 0 bits. */
static inline void unpack_row(const uint8_t* packed, uint32_t row, uint32_t width, uint64x2_t mask,
                              uint64x2_t* v) {
    uint64_t bit_position = (uint64_t)row * width;
    uint32_t offset = (uint32_t)(bit_position % 64u);
    const uint8_t* low = packed + (bit_position / 64u) * SX_BITPACK_ROW_BYTES;
    int64x2_t right = vdupq_n_s64(-(int64_t)offset);
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        v[q] = vshlq_u64(load_bytes(low + q * sizeof(uint64x2_t)), right);
    }
    if (offset + width > 64u) {
        /* The values continue in the next word of each lane. */
        int64x2_t left = vdupq_n_s64((int64_t)(64u - offset));
        const uint8_t* high = low + SX_BITPACK_ROW_BYTES;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            v[q] = vorrq_u64(v[q], vshlq_u64(load_bytes(high + q * sizeof(uint64x2_t)), left));
        }
    }
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        v[q] = vandq_u64(v[q], mask);
    }
}

static inline void unpack_or_zero_row(const uint8_t* packed, uint32_t row, uint32_t width,
                                      uint64x2_t mask, uint64x2_t* v) {
    if (width == 0) {
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            v[q] = vdupq_n_u64(0);
        }
    } else {
        unpack_row(packed, row, width, mask, v);
    }
}

/* Low 64 bits of v * multiplier, from 32-bit halves: lo*lo + ((hi*lo + lo*hi) << 32). */
static inline uint64x2_t multiply_u64(uint64x2_t v, uint32x2_t multiplier_low,
                                      uint32x2_t multiplier_high) {
    uint32x2_t v_low = vmovn_u64(v);
    uint32x2_t v_high = vshrn_n_u64(v, 32);
    uint32x2_t cross = vadd_u32(vmul_u32(v_high, multiplier_low), vmul_u32(v_low, multiplier_high));
    return vaddq_u64(vmull_u32(v_low, multiplier_low), vshlq_n_u64(vmovl_u32(cross), 32));
}

/*
 * Rows are packed in order, so each lane's current output word is kept in a register and
 * stored once it is full, instead of being read and written back for every row.
 */
static void bitpack_neon(const uint64_t* values, uint32_t count, uint32_t width, uint64_t base,
                         uint8_t* out) {
    if (width == 0) {
        return; /* nothing is stored */
    }
    uint64x2_t mask = vdupq_n_u64(sx_low_mask_u64(width));
    uint64x2_t base_vector = vdupq_n_u64(base);
    uint64x2_t words[VECTORS_PER_ROW];
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        words[q] = vdupq_n_u64(0);
    }
    uint8_t* next_words = out;
    uint64_t last_row[SX_BITPACK_LANES];
    uint32_t rows = sx_bitpack_rows(count);
    for (uint32_t row = 0; row < rows; row++) {
        const uint64_t* src = values + (size_t)row * SX_BITPACK_LANES;
        if ((row + 1u) * SX_BITPACK_LANES > count) {
            /* Padding positions hold the base, so they pack as zero. */
            for (uint32_t lane = 0; lane < SX_BITPACK_LANES; lane++) {
                uint32_t index = row * SX_BITPACK_LANES + lane;
                last_row[lane] = index < count ? values[index] : base;
            }
            src = last_row;
        }
        uint32_t offset = (uint32_t)(((uint64_t)row * width) % 64u);
        int64x2_t left = vdupq_n_s64((int64_t)offset);
        uint64x2_t v[VECTORS_PER_ROW];
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            v[q] = vandq_u64(vsubq_u64(vld1q_u64(src + q * VECTOR_LANES), base_vector), mask);
            words[q] = vorrq_u64(words[q], vshlq_u64(v[q], left));
        }
        if (offset + width >= 64u) {
            /* The words are full; the next ones start with the bits that did not fit
             * (a shift by 64 yields zero). */
            int64x2_t right = vdupq_n_s64(-(int64_t)(64u - offset));
            for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
                store_bytes(next_words + q * sizeof(uint64x2_t), words[q]);
                words[q] = vshlq_u64(v[q], right);
            }
            next_words += SX_BITPACK_ROW_BYTES;
        }
    }
    if (next_words < out + sx_bitpack_size(count, width)) {
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            store_bytes(next_words + q * sizeof(uint64x2_t), words[q]);
        }
    }
}

static void bitunpack_neon(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t* out) {
    uint64x2_t mask = vdupq_n_u64(sx_low_mask_u64(width));
    uint64x2_t v[VECTORS_PER_ROW];
    for (uint32_t row = 0; row < sx_bitpack_rows(count); row++) {
        unpack_or_zero_row(packed, row, width, mask, v);
        uint64_t* dst = out + (size_t)row * SX_BITPACK_LANES;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            vst1q_u64(dst + q * VECTOR_LANES, v[q]);
        }
    }
}

static void decode_for_neon(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
                            uint64_t multiplier, uint64_t* out) {
    uint64x2_t mask = vdupq_n_u64(sx_low_mask_u64(width));
    uint64x2_t base_vector = vdupq_n_u64(base);
    uint32x2_t multiplier_low = vdup_n_u32((uint32_t)multiplier);
    uint32x2_t multiplier_high = vdup_n_u32((uint32_t)(multiplier >> 32));
    /* Offsets below 2^32 times a multiplier below 2^32 need one 32x32->64 product. */
    bool narrow = width <= 32u && multiplier <= UINT32_MAX;
    uint64x2_t v[VECTORS_PER_ROW];
    for (uint32_t row = 0; row < sx_bitpack_rows(count); row++) {
        unpack_or_zero_row(packed, row, width, mask, v);
        uint64_t* dst = out + (size_t)row * SX_BITPACK_LANES;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            uint64x2_t product;
            if (multiplier == 1) {
                product = v[q];
            } else if (narrow) {
                product = vmull_u32(vmovn_u64(v[q]), multiplier_low);
            } else {
                product = multiply_u64(v[q], multiplier_low, multiplier_high);
            }
            vst1q_u64(dst + q * VECTOR_LANES, vaddq_u64(product, base_vector));
        }
    }
}

static void decode_alp_neon(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
                            const sx_alp_scales* scales, double* out) {
    uint64x2_t mask = vdupq_n_u64(sx_low_mask_u64(width));
    uint64x2_t shifted_base = vdupq_n_u64(base + SX_ALP_MAGIC_BITS);
    float64x2_t magic = vdupq_n_f64(SX_ALP_MAGIC);
    float64x2_t factor = vdupq_n_f64(scales->decode_factor);
    float64x2_t exponent = vdupq_n_f64(scales->decode_exponent);
    uint64x2_t v[VECTORS_PER_ROW];
    for (uint32_t row = 0; row < sx_bitpack_rows(count); row++) {
        unpack_or_zero_row(packed, row, width, mask, v);
        double* dst = out + (size_t)row * SX_BITPACK_LANES;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            /* (base + offset) + MAGIC_BITS, read as a double, is digits + MAGIC. */
            float64x2_t x = vreinterpretq_f64_u64(vaddq_u64(v[q], shifted_base));
            x = vmulq_f64(vmulq_f64(vsubq_f64(x, magic), factor), exponent);
            vst1q_f64(dst + q * VECTOR_LANES, x);
        }
    }
}

/*
 * Each lane holds a run of consecutive slots, so the lanes are summed independently:
 * running sums go down the rows (two rows at a time), starting from each lane's first
 * value, and each 2x2 block is transposed so that a lane's two values are adjacent.
 */
static void rebuild_deltas_neon(const uint64_t* deltas, uint32_t slot_count, uint64_t first,
                                uint64_t* out) {
    uint32_t rows = sx_bitpack_rows(slot_count);
    uint64x2_t sums[VECTORS_PER_ROW];
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        sums[q] = vdupq_n_u64(0);
    }
    for (uint32_t row = 0; row < rows; row++) {
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            sums[q] = vaddq_u64(
                sums[q], vld1q_u64(deltas + (size_t)row * SX_BITPACK_LANES + q * VECTOR_LANES));
        }
    }
    /* Lane l starts after all slots of the lanes before it. */
    uint64_t lane_totals[SX_BITPACK_LANES];
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        vst1q_u64(lane_totals + q * VECTOR_LANES, sums[q]);
    }
    uint64_t lane_starts[SX_BITPACK_LANES];
    uint32_t lane_slots[SX_BITPACK_LANES];
    uint64_t start = first;
    for (uint32_t lane = 0; lane < SX_BITPACK_LANES; lane++) {
        uint32_t before = lane * rows;
        lane_starts[lane] = start;
        lane_slots[lane] = 0;
        if (slot_count > before) {
            lane_slots[lane] = slot_count - before < rows ? slot_count - before : rows;
        }
        start += lane_totals[lane];
    }

    uint32_t full_lanes = slot_count / rows;

    out[0] = first;
    uint64_t* values = out + 1;
    uint64x2_t running[VECTORS_PER_ROW];
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        running[q] = vld1q_u64(lane_starts + q * VECTOR_LANES);
    }
    for (uint32_t row = 0; row < rows; row += 2u) {
        const uint64_t* row0 = deltas + (size_t)row * SX_BITPACK_LANES;
        const uint64_t* row1 = row0 + SX_BITPACK_LANES;
        bool second_row = row + 1u < rows;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            uint64x2_t sum0 = vaddq_u64(running[q], vld1q_u64(row0 + q * VECTOR_LANES));
            uint64x2_t sum1 =
                second_row ? vaddq_u64(sum0, vld1q_u64(row1 + q * VECTOR_LANES)) : sum0;
            running[q] = sum1;
            /* lane_values[k] holds rows `row` and `row + 1` of lane 2q + k. */
            uint64x2_t lane_values[2] = {vzip1q_u64(sum0, sum1), vzip2q_u64(sum0, sum1)};
            uint64_t* dst = values + (size_t)(q * VECTOR_LANES) * rows + row;
            if (second_row && q * VECTOR_LANES + 2u <= full_lanes) {
                vst1q_u64(dst, lane_values[0]);
                vst1q_u64(dst + rows, lane_values[1]);
                continue;
            }
            for (uint32_t k = 0; k < 2u; k++) {
                uint32_t slots = lane_slots[q * VECTOR_LANES + k];
                if (slots >= row + 2u) {
                    vst1q_u64(dst + (size_t)k * rows, lane_values[k]);
                } else if (slots == row + 1u) {
                    vst1q_lane_u64(dst + (size_t)k * rows, lane_values[k], 0);
                }
            }
        }
    }
}

typedef struct alp_constants {
    float64x2_t encode_exponent;
    float64x2_t encode_factor;
    float64x2_t decode_factor;
    float64x2_t decode_exponent;
    float64x2_t magic;
    uint64x2_t magic_bits;
    float64x2_t upper;
    float64x2_t lower;
} alp_constants;

/* Encodes two values; `kept` is all ones in the lanes that round-trip. */
static inline int64x2_t encode_two(const double* values, const alp_constants* c, uint64x2_t* kept) {
    float64x2_t value = vld1q_f64(values);
    float64x2_t scaled = vmulq_f64(vmulq_f64(value, c->encode_exponent), c->encode_factor);
    float64x2_t shifted = vaddq_f64(scaled, c->magic);
    float64x2_t decoded =
        vmulq_f64(vmulq_f64(vsubq_f64(shifted, c->magic), c->decode_factor), c->decode_exponent);
    uint64x2_t in_range = vandq_u64(vcgtq_f64(scaled, c->lower), vcltq_f64(scaled, c->upper));
    *kept = vandq_u64(in_range,
                      vceqq_u64(vreinterpretq_u64_f64(decoded), vreinterpretq_u64_f64(value)));
    return vreinterpretq_s64_u64(vsubq_u64(vreinterpretq_u64_f64(shifted), c->magic_bits));
}

/* Kept bits of eight lanes (bit k set where lane k is kept). */
static inline uint8_t kept_byte(const uint64x2_t* kept) {
    static const uint8_t weights[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    uint32x4_t low = vuzp1q_u32(vreinterpretq_u32_u64(kept[0]), vreinterpretq_u32_u64(kept[1]));
    uint32x4_t high = vuzp1q_u32(vreinterpretq_u32_u64(kept[2]), vreinterpretq_u32_u64(kept[3]));
    uint8x8_t lanes =
        vmovn_u16(vuzp1q_u16(vreinterpretq_u16_u32(low), vreinterpretq_u16_u32(high)));
    return vaddv_u8(vand_u8(vld1_u8(weights), lanes));
}

/* Lane-wise minimum and maximum of 64-bit integers (NEON has no instruction for them). */
static inline int64x2_t min_i64x2(int64x2_t a, int64x2_t b) {
    return vbslq_s64(vcgtq_s64(a, b), b, a);
}

static inline int64x2_t max_i64x2(int64x2_t a, int64x2_t b) {
    return vbslq_s64(vcgtq_s64(b, a), b, a);
}

/*
 * The digit range in a second pass (see kernels_alp.h): the values of a word without
 * exceptions are scanned with four independent minimum and maximum vectors, the values of
 * other words one by one.
 */
static void digit_range_neon(const int64_t* digits, uint32_t count, const uint64_t* exception_bits,
                             sx_alp_digit_range* range) {
    int64x2_t min[4];
    int64x2_t max[4];
    for (uint32_t k = 0; k < 4u; k++) {
        min[k] = vdupq_n_s64(INT64_MAX);
        max[k] = vdupq_n_s64(INT64_MIN);
    }
    range->min = INT64_MAX;
    range->max = INT64_MIN;
    for (uint32_t word = 0; word * 64u < count; word++) {
        uint32_t begin = word * 64u;
        uint32_t end = count - begin < 64u ? count : begin + 64u;
        if (exception_bits[word] != 0) {
            sx_alp_range_add_kept(digits, begin, end, exception_bits[word], range);
            continue;
        }
        uint32_t i = begin;
        for (; i + 8u <= end; i += 8u) {
            for (uint32_t k = 0; k < 4u; k++) {
                int64x2_t v = vld1q_s64(digits + i + k * VECTOR_LANES);
                min[k] = min_i64x2(min[k], v);
                max[k] = max_i64x2(max[k], v);
            }
        }
        sx_alp_range_add_kept(digits, i, end, 0, range);
    }
    int64x2_t lowest = min_i64x2(min_i64x2(min[0], min[1]), min_i64x2(min[2], min[3]));
    int64x2_t highest = max_i64x2(max_i64x2(max[0], max[1]), max_i64x2(max[2], max[3]));
    range->min = min_i64(range->min, min_i64(vgetq_lane_s64(lowest, 0), vgetq_lane_s64(lowest, 1)));
    range->max =
        max_i64(range->max, max_i64(vgetq_lane_s64(highest, 0), vgetq_lane_s64(highest, 1)));
}

static uint32_t encode_alp_neon(const double* values, uint32_t count, const sx_alp_scales* scales,
                                int64_t* digits, uint64_t* exception_bits,
                                sx_alp_digit_range* range) {
    alp_constants c = {
        .encode_exponent = vdupq_n_f64(scales->encode_exponent),
        .encode_factor = vdupq_n_f64(scales->encode_factor),
        .decode_factor = vdupq_n_f64(scales->decode_factor),
        .decode_exponent = vdupq_n_f64(scales->decode_exponent),
        .magic = vdupq_n_f64(SX_ALP_MAGIC),
        .magic_bits = vdupq_n_u64(SX_ALP_MAGIC_BITS),
        .upper = vdupq_n_f64(SX_ALP_ROUND_LIMIT),
        .lower = vdupq_n_f64(-SX_ALP_ROUND_LIMIT),
    };
    uint8_t kept_bytes[SX_VECTOR_SIZE / 8];
    uint32_t i = 0;
    for (; i + 8u <= count; i += 8u) {
        uint64x2_t kept[4];
        for (uint32_t k = 0; k < 4u; k++) {
            vst1q_s64(digits + i + k * VECTOR_LANES,
                      encode_two(values + i + k * VECTOR_LANES, &c, &kept[k]));
        }
        kept_bytes[i / 8u] = kept_byte(kept);
    }
    if (i < count) {
        kept_bytes[i / 8u] = sx_alp_encode_group(values, i, count, scales, digits);
    }
    uint32_t exceptions = sx_alp_exceptions_from_kept(kept_bytes, count, exception_bits);
    digit_range_neon(digits, count, exception_bits, range);
    return exceptions;
}

const sx_codec_kernels sx_kernels_neon = {
    .level = SX_SIMD_NEON,
    .bitpack = bitpack_neon,
    .bitunpack = bitunpack_neon,
    .decode_for = decode_for_neon,
    .decode_alp = decode_alp_neon,
    .rebuild_deltas = rebuild_deltas_neon,
    .encode_alp = encode_alp_neon,
};

#endif /* SX_ARCH_ARM64 */
