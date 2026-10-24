/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * NEON codec kernels (ARM64). A row of 16 values is eight 128-bit vectors of two lanes.
 * Every function computes exactly what its portable counterpart in kernels_scalar.c
 * computes; see kernels.h for the contracts.
 */

#include "encoding/bitpack.h"
#include "encoding/kernels.h"
#include "encoding/kernels_alp.h"
#include "encoding/kernels_int.h"

#include "common/bits.h"
#include "common/compiler.h"

#include <string.h>

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

/* Broadcast constants of frame-of-reference decoding. */
typedef struct for_constants {
    uint64x2_t mask;
    uint64x2_t base;
    uint32x2_t multiplier_low;
    uint32x2_t multiplier_high;
} for_constants;

static inline for_constants make_for_constants(uint32_t width, uint64_t base, uint64_t multiplier) {
    for_constants c = {
        .mask = vdupq_n_u64(sx_low_mask_u64(width)),
        .base = vdupq_n_u64(base),
        .multiplier_low = vdup_n_u32((uint32_t)multiplier),
        .multiplier_high = vdup_n_u32((uint32_t)(multiplier >> 32)),
    };
    return c;
}

/* Decodes row `row`: base + unpacked * multiplier. */
static SX_ALWAYS_INLINE void decode_row(const uint8_t* packed, uint32_t row, uint32_t width,
                                        const for_constants* c, sx_multiply_kind kind,
                                        uint64x2_t* v) {
    unpack_or_zero_row(packed, row, width, c->mask, v);
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        if (kind == SX_MULTIPLY_NARROW) {
            v[q] = vmull_u32(vmovn_u64(v[q]), c->multiplier_low);
        } else if (kind == SX_MULTIPLY_WIDE) {
            v[q] = multiply_u64(v[q], c->multiplier_low, c->multiplier_high);
        }
        v[q] = vaddq_u64(v[q], c->base);
    }
}

static SX_ALWAYS_INLINE void decode_for_rows(const uint8_t* packed, uint32_t count, uint32_t width,
                                             const for_constants* c, sx_multiply_kind kind,
                                             uint64_t* out) {
    uint64x2_t v[VECTORS_PER_ROW];
    for (uint32_t row = 0; row < sx_bitpack_rows(count); row++) {
        decode_row(packed, row, width, c, kind, v);
        uint64_t* dst = out + (size_t)row * SX_BITPACK_LANES;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            vst1q_u64(dst + q * VECTOR_LANES, v[q]);
        }
    }
}

/* Each kind of multiplication gets its own copy of the loop. */
static void decode_for_neon(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
                            uint64_t multiplier, uint64_t* out) {
    for_constants c = make_for_constants(width, base, multiplier);
    switch (sx_multiply_kind_of(width, multiplier)) {
        case SX_MULTIPLY_NONE:
            decode_for_rows(packed, count, width, &c, SX_MULTIPLY_NONE, out);
            break;
        case SX_MULTIPLY_NARROW:
            decode_for_rows(packed, count, width, &c, SX_MULTIPLY_NARROW, out);
            break;
        default:
            decode_for_rows(packed, count, width, &c, SX_MULTIPLY_WIDE, out);
            break;
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

static void zero_rows(uint64_t* rows, uint32_t count) {
    for (uint32_t i = 0; i < count * VECTORS_PER_ROW; i++) {
        vst1q_u64(rows + i * VECTOR_LANES, vdupq_n_u64(0));
    }
}

/* Decodes the deltas into scratch rows 1 to `rows` and sums up each lane on the way. */
static SX_ALWAYS_INLINE void decode_deltas_rows(const uint8_t* packed, uint32_t rows,
                                                uint32_t width, const for_constants* c,
                                                sx_multiply_kind kind, uint64_t* scratch,
                                                uint64_t* lane_totals) {
    uint64x2_t totals[VECTORS_PER_ROW];
    uint64x2_t v[VECTORS_PER_ROW];
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        totals[q] = vdupq_n_u64(0);
    }
    uint64_t* dst = scratch + SX_BITPACK_LANES;
    for (uint32_t row = 0; row < rows; row++) {
        decode_row(packed, row, width, c, kind, v);
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            vst1q_u64(dst + q * VECTOR_LANES, v[q]);
            totals[q] = vaddq_u64(totals[q], v[q]);
        }
        dst += SX_BITPACK_LANES;
    }
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        vst1q_u64(lane_totals + q * VECTOR_LANES, totals[q]);
    }
}

static void decode_deltas_neon(const uint8_t* packed, uint32_t slot_count, uint32_t width,
                               uint64_t base, uint64_t multiplier, uint64_t* scratch,
                               uint64_t* lane_totals) {
    uint32_t rows = sx_bitpack_rows(slot_count);
    for_constants c = make_for_constants(width, base, multiplier);
    zero_rows(scratch, 1);
    switch (sx_multiply_kind_of(width, multiplier)) {
        case SX_MULTIPLY_NONE:
            decode_deltas_rows(packed, rows, width, &c, SX_MULTIPLY_NONE, scratch, lane_totals);
            break;
        case SX_MULTIPLY_NARROW:
            decode_deltas_rows(packed, rows, width, &c, SX_MULTIPLY_NARROW, scratch, lane_totals);
            break;
        default:
            decode_deltas_rows(packed, rows, width, &c, SX_MULTIPLY_WIDE, scratch, lane_totals);
            break;
    }
    uint32_t last_row = sx_delta_scratch_last_row(rows);
    if (last_row > rows) {
        zero_rows(scratch + (size_t)(rows + 1u) * SX_BITPACK_LANES, last_row - rows);
    }
}

/*
 * Each lane holds a run of consecutive slots, so the lanes are summed independently:
 * running sums go down the rows (two rows at a time) from each lane's start, and each
 * 2x2 block is transposed so that a lane's two values are adjacent. A last single row
 * is stored value by value.
 */
static void rebuild_deltas_neon(const uint64_t* scratch, uint32_t slot_count, uint64_t first,
                                const uint64_t* lane_totals, uint64_t* out) {
    uint32_t rows = sx_bitpack_rows(slot_count);
    uint64_t starts[SX_BITPACK_LANES];
    uint64_t end = sx_delta_lane_starts(lane_totals, first, starts);
    uint64x2_t running[VECTORS_PER_ROW];
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        running[q] = vld1q_u64(starts + q * VECTOR_LANES);
    }
    const uint64_t* src = scratch;
    uint32_t j = 0;
    for (; j + 2u <= rows; j += 2u) {
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            uint64x2_t row0 = vaddq_u64(running[q], vld1q_u64(src + q * VECTOR_LANES));
            uint64x2_t row1 = vaddq_u64(row0, vld1q_u64(src + SX_BITPACK_LANES + q * VECTOR_LANES));
            running[q] = row1;
            /* Lanes 2q and 2q + 1, rows j and j + 1. */
            uint64_t* dst = out + (size_t)(q * VECTOR_LANES) * rows + j;
            vst1q_u64(dst, vzip1q_u64(row0, row1));
            vst1q_u64(dst + rows, vzip2q_u64(row0, row1));
        }
        src += 2u * SX_BITPACK_LANES;
    }
    if (j < rows) {
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            uint64x2_t row0 = vaddq_u64(running[q], vld1q_u64(src + q * VECTOR_LANES));
            uint64_t* dst = out + (size_t)(q * VECTOR_LANES) * rows + j;
            vst1q_lane_u64(dst, row0, 0);
            vst1q_lane_u64(dst + rows, row0, 1);
        }
    }
    if (slot_count == SX_BITPACK_LANES * rows) {
        out[SX_BITPACK_LANES * rows] = end;
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

/*
 * Each vector of values is subtracted from the same values loaded one element earlier,
 * which gives two deltas; the last value, if any, is summarized alone.
 */
static void summarize_ints_neon(const int64_t* values, uint32_t count, uint64_t* deltas,
                                sx_int_summary* summary) {
    sx_summary_start(values, count, summary);
    int64x2_t min = vdupq_n_s64(summary->min);
    int64x2_t max = min;
    int64x2_t delta_min = vdupq_n_s64(summary->delta_min);
    int64x2_t delta_max = delta_min;
    uint64x2_t equal = vdupq_n_u64(0);
    uint32_t i = 1;
    for (; i + VECTOR_LANES <= count; i += VECTOR_LANES) {
        int64x2_t v = vld1q_s64(values + i);
        int64x2_t previous = vld1q_s64(values + i - 1);
        int64x2_t delta = vsubq_s64(v, previous);
        vst1q_u64(deltas + i - 1, vreinterpretq_u64_s64(delta));
        min = min_i64x2(min, v);
        max = max_i64x2(max, v);
        delta_min = min_i64x2(delta_min, delta);
        delta_max = max_i64x2(delta_max, delta);
        /* An equal lane is all ones, minus one. */
        equal = vsubq_u64(equal, vceqq_s64(v, previous));
    }
    summary->min = min_i64(summary->min, min_i64(vgetq_lane_s64(min, 0), vgetq_lane_s64(min, 1)));
    summary->max = max_i64(summary->max, max_i64(vgetq_lane_s64(max, 0), vgetq_lane_s64(max, 1)));
    summary->delta_min = min_i64(
        summary->delta_min, min_i64(vgetq_lane_s64(delta_min, 0), vgetq_lane_s64(delta_min, 1)));
    summary->delta_max = max_i64(
        summary->delta_max, max_i64(vgetq_lane_s64(delta_max, 0), vgetq_lane_s64(delta_max, 1)));
    summary->changes = (i - 1u) - (uint32_t)vaddvq_u64(equal);
    sx_summarize_ints_from(values, i, count, deltas, summary);
}

/* The constants of a multiple test, in vectors. */
typedef struct multiple_vectors {
    uint64x2_t base;
    int64x2_t right;
    uint32x2_t inverse_low;
    uint32x2_t inverse_high;
    uint64x2_t low_bits;
    uint64x2_t max_quotient;
} multiple_vectors;

/*
 * Blocks of eight numbers are tested with one branch; the rest one by one. Zero is a
 * multiple of every divisor, so blocks of zeros skip the multiplications.
 */
static uint32_t first_non_multiple_neon(const uint64_t* numbers, uint32_t start, uint32_t count,
                                        uint64_t base, const sx_multiple_test* test,
                                        uint64_t* quotients) {
    multiple_vectors t = {
        .base = vdupq_n_u64(base),
        .right = vdupq_n_s64(-(int64_t)test->exact.shift),
        .inverse_low = vdup_n_u32((uint32_t)test->exact.inverse),
        .inverse_high = vdup_n_u32((uint32_t)(test->exact.inverse >> 32)),
        .low_bits = vdupq_n_u64(test->low_bits),
        .max_quotient = vdupq_n_u64(test->max_quotient),
    };
    bool multiply = test->max_quotient != UINT64_MAX;
    uint32_t i = start;
    for (; i + 4u * VECTOR_LANES <= count; i += 4u * VECTOR_LANES) {
        uint64x2_t x[4];
        uint64x2_t any = vdupq_n_u64(0);
        for (uint32_t q = 0; q < 4u; q++) {
            x[q] = vsubq_u64(vld1q_u64(numbers + i + q * VECTOR_LANES), t.base);
            any = vorrq_u64(any, x[q]);
        }
        if (vmaxvq_u32(vreinterpretq_u32_u64(any)) == 0) {
            for (uint32_t q = 0; q < 4u; q++) {
                vst1q_u64(quotients + i + q * VECTOR_LANES, vdupq_n_u64(0));
            }
            continue;
        }
        uint64x2_t misses = vdupq_n_u64(0);
        for (uint32_t q = 0; q < 4u; q++) {
            uint64x2_t quotient = vshlq_u64(x[q], t.right);
            misses = vorrq_u64(misses, vtstq_u64(x[q], t.low_bits));
            if (multiply) {
                quotient = multiply_u64(quotient, t.inverse_low, t.inverse_high);
                misses = vorrq_u64(misses, vcgtq_u64(quotient, t.max_quotient));
            }
            vst1q_u64(quotients + i + q * VECTOR_LANES, quotient);
        }
        if (vmaxvq_u32(vreinterpretq_u32_u64(misses)) != 0) {
            break;
        }
    }
    return sx_first_non_multiple_from(numbers, i, count, base, test, quotients);
}

static void divide_exact_neon(const uint64_t* numbers, uint32_t count, uint64_t base,
                              const sx_exact_divisor* divisor, uint64_t* out) {
    uint64x2_t base_vector = vdupq_n_u64(base);
    int64x2_t right = vdupq_n_s64(-(int64_t)divisor->shift);
    uint32x2_t inverse_low = vdup_n_u32((uint32_t)divisor->inverse);
    uint32x2_t inverse_high = vdup_n_u32((uint32_t)(divisor->inverse >> 32));
    bool multiply = divisor->inverse != 1;
    uint32_t i = 0;
    for (; i + VECTOR_LANES <= count; i += VECTOR_LANES) {
        uint64x2_t quotient = vshlq_u64(vsubq_u64(vld1q_u64(numbers + i), base_vector), right);
        if (multiply) {
            quotient = multiply_u64(quotient, inverse_low, inverse_high);
        }
        vst1q_u64(out + i, quotient);
    }
    for (; i < count; i++) {
        out[i] = sx_divide_exact(numbers[i] - base, divisor);
    }
}

/* NEON has no leading zero count of 64-bit lanes; the scalar histogram is as fast. */
static uint32_t width_counts_neon(const uint64_t* numbers, uint32_t count, uint32_t max_width,
                                  uint32_t enough, uint32_t* counts) {
    memset(counts, 0, ((size_t)max_width + 1u) * sizeof(uint32_t));
    sx_add_width_counts(numbers, count, max_width, counts);
    return sx_width_counts_stop(counts, max_width, enough);
}

/* Two lanes per vector gain nothing over building each word of bits with scalar code. */
static uint32_t find_exceptions_neon(const uint64_t* offsets, uint32_t count, uint64_t limit,
                                     uint64_t* bits) {
    uint32_t exceptions = 0;
    for (uint32_t word = 0; word * 64u < count; word++) {
        uint32_t begin = word * 64u;
        uint32_t end = count - begin < 64u ? count : begin + 64u;
        bits[word] = sx_exception_word(offsets, begin, end, limit);
        exceptions += sx_popcount_u64(bits[word]);
    }
    return exceptions;
}

/* The values of `lane` in rows [row, row + 2): zero where the lane has none. */
static inline uint64x2_t lane_pair(const uint64_t* values, uint32_t count, uint32_t rows,
                                   uint32_t lane, uint32_t row) {
    uint32_t start = lane * rows + row;
    uint32_t in_lane = rows - row < 2u ? rows - row : 2u;
    uint32_t available = start < count ? count - start : 0;
    uint32_t present = available < in_lane ? available : in_lane;
    if (present == 2u) {
        return vld1q_u64(values + start);
    }
    if (present == 1u) {
        return vsetq_lane_u64(values[start], vdupq_n_u64(0), 0);
    }
    return vdupq_n_u64(0);
}

/*
 * Each 2x2 block of the output (two rows of two lanes) is transposed from two runs of two
 * values, one per lane. Blocks of full lanes are loaded directly. Values above the limit
 * are cleared before the transposition, unless there is no limit.
 */
static void to_lane_order_neon(const uint64_t* values, uint32_t count, uint64_t limit,
                               uint64_t* out) {
    uint64x2_t limit_vector = vdupq_n_u64(limit);
    bool clear = limit != UINT64_MAX;
    uint32_t rows = sx_bitpack_rows(count);
    for (uint32_t lane = 0; lane < SX_BITPACK_LANES; lane += VECTOR_LANES) {
        for (uint32_t row = 0; row < rows; row += 2u) {
            uint64x2_t first;
            uint64x2_t second;
            if (row + 2u <= rows && (lane + 1u) * rows + row + 2u <= count) {
                first = vld1q_u64(values + (size_t)lane * rows + row);
                second = vld1q_u64(values + (size_t)(lane + 1u) * rows + row);
            } else {
                first = lane_pair(values, count, rows, lane, row);
                second = lane_pair(values, count, rows, lane + 1u, row);
            }
            if (clear) {
                first = vbicq_u64(first, vcgtq_u64(first, limit_vector));
                second = vbicq_u64(second, vcgtq_u64(second, limit_vector));
            }
            uint64_t* dst = out + (size_t)row * SX_BITPACK_LANES + lane;
            vst1q_u64(dst, vzip1q_u64(first, second));
            if (row + 1u < rows) {
                vst1q_u64(dst + SX_BITPACK_LANES, vzip2q_u64(first, second));
            }
        }
    }
}

const sx_codec_kernels sx_kernels_neon = {
    .level = SX_SIMD_NEON,
    .bitpack = bitpack_neon,
    .bitunpack = bitunpack_neon,
    .decode_for = decode_for_neon,
    .decode_alp = decode_alp_neon,
    .decode_deltas = decode_deltas_neon,
    .rebuild_deltas = rebuild_deltas_neon,
    .encode_alp = encode_alp_neon,
    .summarize_ints = summarize_ints_neon,
    .first_non_multiple = first_non_multiple_neon,
    .divide_exact = divide_exact_neon,
    .width_counts = width_counts_neon,
    .find_exceptions = find_exceptions_neon,
    .to_lane_order = to_lane_order_neon,
};

#endif /* SX_ARCH_ARM64 */
