/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * AVX-512 codec kernels (x86_64, the x86-64-v4 level: AVX-512 F, CD, BW, DQ, and VL, plus
 * the v3 integer extensions). This file is compiled for those instruction sets only (see
 * src/CMakeLists.txt) and is only called after the CPU check in kernels.c. A row of 16
 * values is two 512-bit vectors of eight lanes. Every function computes exactly what its
 * portable counterpart in kernels_scalar.c computes; see kernels.h for the contracts.
 */

#include "encoding/bitpack.h"
#include "encoding/kernels.h"
#include "encoding/kernels_int.h"

#include "common/bits.h"
#include "common/compiler.h"

#if defined(SX_ARCH_X86_64)

#include <immintrin.h>

#define VECTORS_PER_ROW 2u
#define VECTOR_LANES    ((size_t)8)

/*
 * Unpacks row `row` of a block packed at `width` > 0 bits. The shifts use per-lane
 * counts (vpsrlvq), which unlike shifts by a register count do not compete with the
 * shuffles for the same execution port.
 */
static inline void unpack_row(const uint8_t* packed, uint32_t row, uint32_t width, __m512i mask,
                              __m512i* v) {
    uint64_t bit_position = (uint64_t)row * width;
    uint32_t offset = (uint32_t)(bit_position % 64u);
    const uint8_t* low = packed + (bit_position / 64u) * SX_BITPACK_ROW_BYTES;
    __m512i right = _mm512_set1_epi64(offset);
    v[0] = _mm512_srlv_epi64(_mm512_loadu_si512(low), right);
    v[1] = _mm512_srlv_epi64(_mm512_loadu_si512(low + 64), right);
    if (offset + width > 64u) {
        /* The values continue in the next word of each lane. */
        __m512i left = _mm512_set1_epi64(64u - offset);
        const uint8_t* high = low + SX_BITPACK_ROW_BYTES;
        v[0] = _mm512_or_si512(v[0], _mm512_sllv_epi64(_mm512_loadu_si512(high), left));
        v[1] = _mm512_or_si512(v[1], _mm512_sllv_epi64(_mm512_loadu_si512(high + 64), left));
    }
    v[0] = _mm512_and_si512(v[0], mask);
    v[1] = _mm512_and_si512(v[1], mask);
}

static inline void unpack_or_zero_row(const uint8_t* packed, uint32_t row, uint32_t width,
                                      __m512i mask, __m512i* v) {
    if (width == 0) {
        v[0] = _mm512_setzero_si512();
        v[1] = _mm512_setzero_si512();
    } else {
        unpack_row(packed, row, width, mask, v);
    }
}

/*
 * Rows are packed in order, so each lane's current output word is kept in a register and
 * stored once it is full, instead of being read and written back for every row.
 */
static void bitpack_avx512(const uint64_t* values, uint32_t count, uint32_t width, uint64_t base,
                           uint8_t* out) {
    if (width == 0) {
        return; /* nothing is stored */
    }
    __m512i mask = _mm512_set1_epi64((long long)sx_low_mask_u64(width));
    __m512i base_vector = _mm512_set1_epi64((long long)base);
    __m512i words[VECTORS_PER_ROW] = {_mm512_setzero_si512(), _mm512_setzero_si512()};
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
        __m512i v[VECTORS_PER_ROW];
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            v[q] = _mm512_loadu_si512(src + q * VECTOR_LANES);
            v[q] = _mm512_and_si512(_mm512_sub_epi64(v[q], base_vector), mask);
            words[q] =
                _mm512_or_si512(words[q], _mm512_sllv_epi64(v[q], _mm512_set1_epi64(offset)));
        }
        if (offset + width >= 64u) {
            /* The words are full; the next ones start with the bits that did not fit
             * (a shift by 64 yields zero). */
            __m512i right = _mm512_set1_epi64(64u - offset);
            for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
                _mm512_storeu_si512(next_words + q * sizeof(__m512i), words[q]);
                words[q] = _mm512_srlv_epi64(v[q], right);
            }
            next_words += SX_BITPACK_ROW_BYTES;
        }
    }
    if (next_words < out + sx_bitpack_size(count, width)) {
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            _mm512_storeu_si512(next_words + q * sizeof(__m512i), words[q]);
        }
    }
}

static void bitunpack_avx512(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t* out) {
    __m512i mask = _mm512_set1_epi64((long long)sx_low_mask_u64(width));
    __m512i v[VECTORS_PER_ROW];
    for (uint32_t row = 0; row < sx_bitpack_rows(count); row++) {
        unpack_or_zero_row(packed, row, width, mask, v);
        uint64_t* dst = out + (size_t)row * SX_BITPACK_LANES;
        _mm512_storeu_si512(dst, v[0]);
        _mm512_storeu_si512(dst + VECTOR_LANES, v[1]);
    }
}

/* Broadcast constants of frame-of-reference decoding. */
typedef struct for_constants {
    __m512i mask;
    __m512i base;
    __m512i multiplier;
} for_constants;

static inline for_constants make_for_constants(uint32_t width, uint64_t base, uint64_t multiplier) {
    for_constants c = {
        .mask = _mm512_set1_epi64((long long)sx_low_mask_u64(width)),
        .base = _mm512_set1_epi64((long long)base),
        .multiplier = _mm512_set1_epi64((long long)multiplier),
    };
    return c;
}

/* Decodes row `row`: base + unpacked * multiplier. */
static SX_ALWAYS_INLINE void decode_row(const uint8_t* packed, uint32_t row, uint32_t width,
                                        const for_constants* c, sx_multiply_kind kind, __m512i* v) {
    unpack_or_zero_row(packed, row, width, c->mask, v);
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        if (kind == SX_MULTIPLY_NARROW) {
            v[q] = _mm512_mul_epu32(v[q], c->multiplier);
        } else if (kind == SX_MULTIPLY_WIDE) {
            v[q] = _mm512_mullo_epi64(v[q], c->multiplier);
        }
        v[q] = _mm512_add_epi64(v[q], c->base);
    }
}

static SX_ALWAYS_INLINE void decode_for_rows(const uint8_t* packed, uint32_t count, uint32_t width,
                                             const for_constants* c, sx_multiply_kind kind,
                                             uint64_t* out) {
    __m512i v[VECTORS_PER_ROW];
    for (uint32_t row = 0; row < sx_bitpack_rows(count); row++) {
        decode_row(packed, row, width, c, kind, v);
        uint64_t* dst = out + (size_t)row * SX_BITPACK_LANES;
        _mm512_storeu_si512(dst, v[0]);
        _mm512_storeu_si512(dst + VECTOR_LANES, v[1]);
    }
}

/* Each kind of multiplication gets its own copy of the loop. */
static void decode_for_avx512(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
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

static void decode_alp_avx512(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
                              const sx_alp_scales* scales, double* out) {
    __m512i mask = _mm512_set1_epi64((long long)sx_low_mask_u64(width));
    uint64_t magic_base = base + SX_ALP_MAGIC_BITS;
    __m512i shifted_base = _mm512_set1_epi64((long long)magic_base);
    __m512d magic = _mm512_set1_pd(SX_ALP_MAGIC);
    __m512d factor = _mm512_set1_pd(scales->decode_factor);
    __m512d exponent = _mm512_set1_pd(scales->decode_exponent);
    __m512i v[VECTORS_PER_ROW];
    for (uint32_t row = 0; row < sx_bitpack_rows(count); row++) {
        unpack_or_zero_row(packed, row, width, mask, v);
        double* dst = out + (size_t)row * SX_BITPACK_LANES;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            /* (base + offset) + MAGIC_BITS, read as a double, is digits + MAGIC. */
            __m512d x = _mm512_castsi512_pd(_mm512_add_epi64(v[q], shifted_base));
            x = _mm512_mul_pd(_mm512_mul_pd(_mm512_sub_pd(x, magic), factor), exponent);
            _mm512_storeu_pd(dst + q * VECTOR_LANES, x);
        }
    }
}

/* Transposes an 8x8 block of 64-bit values held in eight vectors, in place. */
static inline void transpose_8x8(__m512i* m) {
    __m512i t0 = _mm512_unpacklo_epi64(m[0], m[1]);
    __m512i t1 = _mm512_unpackhi_epi64(m[0], m[1]);
    __m512i t2 = _mm512_unpacklo_epi64(m[2], m[3]);
    __m512i t3 = _mm512_unpackhi_epi64(m[2], m[3]);
    __m512i t4 = _mm512_unpacklo_epi64(m[4], m[5]);
    __m512i t5 = _mm512_unpackhi_epi64(m[4], m[5]);
    __m512i t6 = _mm512_unpacklo_epi64(m[6], m[7]);
    __m512i t7 = _mm512_unpackhi_epi64(m[6], m[7]);
    __m512i u0 = _mm512_shuffle_i64x2(t0, t2, 0x88);
    __m512i u1 = _mm512_shuffle_i64x2(t0, t2, 0xdd);
    __m512i u2 = _mm512_shuffle_i64x2(t1, t3, 0x88);
    __m512i u3 = _mm512_shuffle_i64x2(t1, t3, 0xdd);
    __m512i u4 = _mm512_shuffle_i64x2(t4, t6, 0x88);
    __m512i u5 = _mm512_shuffle_i64x2(t4, t6, 0xdd);
    __m512i u6 = _mm512_shuffle_i64x2(t5, t7, 0x88);
    __m512i u7 = _mm512_shuffle_i64x2(t5, t7, 0xdd);
    m[0] = _mm512_shuffle_i64x2(u0, u4, 0x88);
    m[1] = _mm512_shuffle_i64x2(u2, u6, 0x88);
    m[2] = _mm512_shuffle_i64x2(u1, u5, 0x88);
    m[3] = _mm512_shuffle_i64x2(u3, u7, 0x88);
    m[4] = _mm512_shuffle_i64x2(u0, u4, 0xdd);
    m[5] = _mm512_shuffle_i64x2(u2, u6, 0xdd);
    m[6] = _mm512_shuffle_i64x2(u1, u5, 0xdd);
    m[7] = _mm512_shuffle_i64x2(u3, u7, 0xdd);
}

static void zero_rows(uint64_t* rows, uint32_t count) {
    for (uint32_t i = 0; i < count * VECTORS_PER_ROW; i++) {
        _mm512_storeu_si512(rows + i * VECTOR_LANES, _mm512_setzero_si512());
    }
}

/* Decodes the deltas into scratch rows 1 to `rows` and sums up each lane on the way. */
static SX_ALWAYS_INLINE void decode_deltas_rows(const uint8_t* packed, uint32_t rows,
                                                uint32_t width, const for_constants* c,
                                                sx_multiply_kind kind, uint64_t* scratch,
                                                uint64_t* lane_totals) {
    __m512i totals[VECTORS_PER_ROW] = {_mm512_setzero_si512(), _mm512_setzero_si512()};
    __m512i v[VECTORS_PER_ROW];
    uint64_t* dst = scratch + SX_BITPACK_LANES;
    for (uint32_t row = 0; row < rows; row++) {
        decode_row(packed, row, width, c, kind, v);
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            _mm512_storeu_si512(dst + q * VECTOR_LANES, v[q]);
            totals[q] = _mm512_add_epi64(totals[q], v[q]);
        }
        dst += SX_BITPACK_LANES;
    }
    _mm512_storeu_si512(lane_totals, totals[0]);
    _mm512_storeu_si512(lane_totals + VECTOR_LANES, totals[1]);
}

static void decode_deltas_avx512(const uint8_t* packed, uint32_t slot_count, uint32_t width,
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
 * running sums go down the rows from each lane's start, and each 8x8 block (eight rows of
 * eight lanes) is transposed so that a lane's eight values become one vector stored in
 * place. A last block of fewer than eight rows is stored with a mask, so that it does not
 * run into the next lane.
 */
static void rebuild_deltas_avx512(const uint64_t* scratch, uint32_t slot_count, uint64_t first,
                                  const uint64_t* lane_totals, uint64_t* out) {
    uint32_t rows = sx_bitpack_rows(slot_count);
    uint64_t starts[SX_BITPACK_LANES];
    uint64_t end = sx_delta_lane_starts(lane_totals, first, starts);
    uint32_t blocks = (rows + 7u) / 8u;
    __mmask8 last_mask = (__mmask8)(0xffu >> (blocks * 8u - rows)); /* rows in the last block */
    for (uint32_t h = 0; h < VECTORS_PER_ROW; h++) {
        __m512i running = _mm512_loadu_si512(starts + h * VECTOR_LANES);
        const uint64_t* src = scratch + h * VECTOR_LANES;
        uint64_t* dst = out + h * VECTOR_LANES * rows;
        for (uint32_t block = 0; block < blocks; block++) {
            __m512i m[8];
            for (uint32_t j = 0; j < 8u; j++) {
                running = _mm512_add_epi64(running, _mm512_loadu_si512(src));
                src += SX_BITPACK_LANES;
                m[j] = running;
            }
            transpose_8x8(m);
            if (block + 1u < blocks) {
                for (uint32_t i = 0; i < 8u; i++) {
                    _mm512_storeu_si512(dst + (size_t)i * rows, m[i]);
                }
            } else {
                for (uint32_t i = 0; i < 8u; i++) {
                    _mm512_mask_storeu_epi64(dst + (size_t)i * rows, last_mask, m[i]);
                }
            }
            dst += 8;
        }
    }
    if (slot_count == SX_BITPACK_LANES * rows) {
        out[(size_t)SX_BITPACK_LANES * rows] = end;
    }
}

/*
 * Values are encoded 64 at a time, one word of exception bits, which is built in a
 * register and stored once.
 */
static uint32_t encode_alp_avx512(const double* values, uint32_t count, const sx_alp_scales* scales,
                                  int64_t* digits, uint64_t* exception_bits,
                                  sx_alp_digit_range* range) {
    __m512d encode_exponent = _mm512_set1_pd(scales->encode_exponent);
    __m512d encode_factor = _mm512_set1_pd(scales->encode_factor);
    __m512d decode_factor = _mm512_set1_pd(scales->decode_factor);
    __m512d decode_exponent = _mm512_set1_pd(scales->decode_exponent);
    __m512d magic = _mm512_set1_pd(SX_ALP_MAGIC);
    __m512i magic_bits = _mm512_set1_epi64((long long)SX_ALP_MAGIC_BITS);
    __m512d upper = _mm512_set1_pd(SX_ALP_ROUND_LIMIT);
    __m512d lower = _mm512_set1_pd(-SX_ALP_ROUND_LIMIT);
    __m512i min = _mm512_set1_epi64(INT64_MAX);
    __m512i max = _mm512_set1_epi64(INT64_MIN);
    int64_t tail_min = INT64_MAX;
    int64_t tail_max = INT64_MIN;
    uint32_t exceptions = 0;

    for (uint32_t word = 0; word * 64u < count; word++) {
        uint32_t begin = word * 64u;
        uint32_t end = count - begin < 64u ? count : begin + 64u;
        uint64_t bits = 0;
        uint32_t i = begin;
        for (; i + VECTOR_LANES <= end; i += VECTOR_LANES) {
            __m512d value = _mm512_loadu_pd(values + i);
            __m512d scaled = _mm512_mul_pd(_mm512_mul_pd(value, encode_exponent), encode_factor);
            __m512d shifted = _mm512_add_pd(scaled, magic);
            __m512i candidate = _mm512_sub_epi64(_mm512_castpd_si512(shifted), magic_bits);
            __m512d decoded = _mm512_mul_pd(
                _mm512_mul_pd(_mm512_sub_pd(shifted, magic), decode_factor), decode_exponent);
            __mmask8 in_range = _mm512_cmp_pd_mask(scaled, lower, _CMP_GT_OQ) &
                                _mm512_cmp_pd_mask(scaled, upper, _CMP_LT_OQ);
            __mmask8 exact =
                _mm512_cmpeq_epi64_mask(_mm512_castpd_si512(decoded), _mm512_castpd_si512(value));
            __mmask8 kept = (__mmask8)(in_range & exact);
            _mm512_storeu_si512(digits + i, candidate);
            bits |= (uint64_t)(uint8_t)~kept << (i - begin);
            min = _mm512_mask_min_epi64(min, kept, min, candidate);
            max = _mm512_mask_max_epi64(max, kept, max, candidate);
        }
        for (; i < end; i++) {
            if (!sx_alp_encode_value(values[i], scales, &digits[i])) {
                bits |= UINT64_C(1) << (i - begin);
                continue;
            }
            if (digits[i] < tail_min) {
                tail_min = digits[i];
            }
            if (digits[i] > tail_max) {
                tail_max = digits[i];
            }
        }
        exception_bits[word] = bits;
        exceptions += sx_popcount_u64(bits);
    }
    int64_t vector_min = _mm512_reduce_min_epi64(min);
    int64_t vector_max = _mm512_reduce_max_epi64(max);
    range->min = vector_min < tail_min ? vector_min : tail_min;
    range->max = vector_max > tail_max ? vector_max : tail_max;
    return exceptions;
}

/* Lanes of a vector that hold one of the `remaining` values from its start. */
static inline __mmask8 first_lanes(uint32_t remaining) {
    return remaining >= VECTOR_LANES ? (__mmask8)0xff : (__mmask8)((1u << remaining) - 1u);
}

/* The running minimums and maximums of summarize_ints. */
typedef struct int_ranges {
    __m512i min;
    __m512i max;
    __m512i delta_min;
    __m512i delta_max;
    uint32_t changes;
} int_ranges;

/* Adds the values of `valid` lanes at values + i, and their deltas, to the ranges. */
static inline void add_to_ranges(const int64_t* values, uint32_t i, __mmask8 valid,
                                 uint64_t* deltas, int_ranges* r) {
    __m512i v = _mm512_maskz_loadu_epi64(valid, values + i);
    __m512i previous = _mm512_maskz_loadu_epi64(valid, values + i - 1);
    __m512i delta = _mm512_sub_epi64(v, previous);
    _mm512_mask_storeu_epi64(deltas + i - 1, valid, delta);
    r->min = _mm512_mask_min_epi64(r->min, valid, r->min, v);
    r->max = _mm512_mask_max_epi64(r->max, valid, r->max, v);
    r->delta_min = _mm512_mask_min_epi64(r->delta_min, valid, r->delta_min, delta);
    r->delta_max = _mm512_mask_max_epi64(r->delta_max, valid, r->delta_max, delta);
    r->changes += sx_popcount_u64(_mm512_mask_cmpneq_epi64_mask(valid, v, previous));
}

/*
 * Each vector of values is subtracted from the same values loaded one element earlier,
 * which gives eight deltas; the last vector is masked.
 */
static void summarize_ints_avx512(const int64_t* values, uint32_t count, uint64_t* deltas,
                                  sx_int_summary* summary) {
    sx_summary_start(values, count, summary);
    int_ranges r = {
        .min = _mm512_set1_epi64(summary->min),
        .max = _mm512_set1_epi64(summary->max),
        .delta_min = _mm512_set1_epi64(summary->delta_min),
        .delta_max = _mm512_set1_epi64(summary->delta_max),
        .changes = 0,
    };
    uint32_t i = 1;
    for (; i + VECTOR_LANES <= count; i += VECTOR_LANES) {
        add_to_ranges(values, i, 0xff, deltas, &r);
    }
    if (i < count) {
        add_to_ranges(values, i, first_lanes(count - i), deltas, &r);
    }
    summary->min = _mm512_reduce_min_epi64(r.min);
    summary->max = _mm512_reduce_max_epi64(r.max);
    summary->delta_min = _mm512_reduce_min_epi64(r.delta_min);
    summary->delta_max = _mm512_reduce_max_epi64(r.delta_max);
    summary->changes = r.changes;
}

/* The constants of a multiple test, in vectors. */
typedef struct multiple_vectors {
    __m512i base;
    __m512i shift;
    __m512i inverse;
    __m512i low_bits;
    __m512i max_quotient;
    bool multiply; /* false if the odd part of the divisor is 1: only low bits matter */
} multiple_vectors;

/*
 * Lanes of `valid` whose number minus the base is not a multiple; writes the quotients of
 * all valid lanes.
 */
static inline __mmask8 multiple_misses(const uint64_t* numbers, __mmask8 valid,
                                       const multiple_vectors* t, uint64_t* quotients) {
    __m512i x = _mm512_sub_epi64(_mm512_maskz_loadu_epi64(valid, numbers), t->base);
    __m512i quotient = _mm512_srlv_epi64(x, t->shift);
    __mmask8 misses = _mm512_mask_test_epi64_mask(valid, x, t->low_bits);
    /* Zero is a multiple of every divisor; vectors of zeros skip the multiplication. */
    if (t->multiply && _mm512_mask_test_epi64_mask(valid, x, x) != 0) {
        quotient = _mm512_mullo_epi64(quotient, t->inverse);
        misses |= _mm512_mask_cmpgt_epu64_mask(valid, quotient, t->max_quotient);
    }
    _mm512_mask_storeu_epi64(quotients, valid, quotient);
    return misses;
}

static uint32_t first_non_multiple_avx512(const uint64_t* numbers, uint32_t start, uint32_t count,
                                          uint64_t base, const sx_multiple_test* test,
                                          uint64_t* quotients) {
    multiple_vectors t = {
        .base = _mm512_set1_epi64((long long)base),
        .shift = _mm512_set1_epi64(test->exact.shift),
        .inverse = _mm512_set1_epi64((long long)test->exact.inverse),
        .low_bits = _mm512_set1_epi64((long long)test->low_bits),
        .max_quotient = _mm512_set1_epi64((long long)test->max_quotient),
        .multiply = test->max_quotient != UINT64_MAX,
    };
    uint32_t i = start;
    for (; i + VECTOR_LANES <= count; i += VECTOR_LANES) {
        uint32_t misses = multiple_misses(numbers + i, 0xff, &t, quotients + i);
        if (misses != 0) {
            return i + sx_trailing_zeros_u64(misses);
        }
    }
    if (i < count) {
        uint32_t misses = multiple_misses(numbers + i, first_lanes(count - i), &t, quotients + i);
        if (misses != 0) {
            return i + sx_trailing_zeros_u64(misses);
        }
    }
    return count;
}

static inline void divide_vector(const uint64_t* numbers, __mmask8 valid, __m512i base,
                                 __m512i shift, const __m512i* inverse, uint64_t* out) {
    __m512i quotient =
        _mm512_srlv_epi64(_mm512_sub_epi64(_mm512_maskz_loadu_epi64(valid, numbers), base), shift);
    if (inverse != NULL) {
        quotient = _mm512_mullo_epi64(quotient, *inverse);
    }
    _mm512_mask_storeu_epi64(out, valid, quotient);
}

static void divide_exact_avx512(const uint64_t* numbers, uint32_t count, uint64_t base,
                                const sx_exact_divisor* divisor, uint64_t* out) {
    __m512i base_vector = _mm512_set1_epi64((long long)base);
    __m512i shift = _mm512_set1_epi64(divisor->shift);
    __m512i inverse_vector = _mm512_set1_epi64((long long)divisor->inverse);
    const __m512i* inverse = divisor->inverse != 1 ? &inverse_vector : NULL;
    uint32_t i = 0;
    for (; i + VECTOR_LANES <= count; i += VECTOR_LANES) {
        divide_vector(numbers + i, 0xff, base_vector, shift, inverse, out + i);
    }
    if (i < count) {
        divide_vector(numbers + i, first_lanes(count - i), base_vector, shift, inverse, out + i);
    }
}

/*
 * The leading zero counts (64 - width) of `present` (1 to 64) numbers, merged into the
 * 64 bytes of one vector: one byte per number, in some order. Missing numbers count as
 * zeros, with 64 leading zeros.
 */
static inline __m512i merged_leading_zeros(const uint64_t* numbers, uint32_t present) {
    __m512i zeros = _mm512_setzero_si512();
    for (uint32_t k = 0; k < 8u; k++) {
        uint32_t first = k * (uint32_t)VECTOR_LANES;
        __m512i x = _mm512_setzero_si512();
        if (present >= first + VECTOR_LANES) {
            x = _mm512_loadu_si512(numbers + first);
        } else if (present > first) {
            x = _mm512_maskz_loadu_epi64(first_lanes(present - first), numbers + first);
        }
        __m512i shift = _mm512_set1_epi64((long long)k * 8);
        zeros = _mm512_or_si512(zeros, _mm512_sllv_epi64(_mm512_lzcnt_epi64(x), shift));
    }
    return zeros;
}

/*
 * The numbers are merged in blocks of 64 (merged_leading_zeros), and then the bytes of all
 * blocks are counted for each width, from max_width down. The missing numbers of the last
 * block count as width 0, which is taken care of at the end.
 */
static uint32_t width_counts_avx512(const uint64_t* numbers, uint32_t count, uint32_t max_width,
                                    uint32_t enough, uint32_t* counts) {
    __m512i blocks[SX_VECTOR_SIZE / 64u];
    uint32_t block_count = 0;
    uint32_t first = 0;
    for (; first + 64u <= count; first += 64u) {
        blocks[block_count] = merged_leading_zeros(numbers + first, 64u);
        block_count++;
    }
    if (first < count) {
        blocks[block_count] = merged_leading_zeros(numbers + first, count - first);
        block_count++;
    }
    uint32_t at_least = 0;
    for (uint32_t width = max_width; width > 0; width--) {
        __m512i target = _mm512_set1_epi8((char)(64u - width));
        uint32_t total = 0;
        for (uint32_t b = 0; b < block_count; b++) {
            total += sx_popcount_u64(_mm512_cmpeq_epi8_mask(blocks[b], target));
        }
        counts[width] = total;
        at_least += total;
        if (at_least >= enough) {
            return width;
        }
    }
    counts[0] = count - at_least;
    return 0;
}

/*
 * Each word of 64 exception bits is built from the comparison masks of eight vectors in a
 * register; the last vector of the last word is masked.
 */
static uint32_t find_exceptions_avx512(const uint64_t* offsets, uint32_t count, uint64_t limit,
                                       uint64_t* bits) {
    __m512i limit_vector = _mm512_set1_epi64((long long)limit);
    uint32_t exceptions = 0;
    for (uint32_t word = 0; word * 64u < count; word++) {
        uint64_t word_bits = 0;
        for (uint32_t k = 0; k < 8u; k++) {
            uint32_t first = word * 64u + k * (uint32_t)VECTOR_LANES;
            if (first >= count) {
                break;
            }
            __mmask8 valid = first_lanes(count - first);
            __m512i x = _mm512_maskz_loadu_epi64(valid, offsets + first);
            __mmask8 wide = _mm512_mask_cmpgt_epu64_mask(valid, x, limit_vector);
            word_bits |= (uint64_t)wide << (8u * k);
        }
        bits[word] = word_bits;
        exceptions += sx_popcount_u64(word_bits);
    }
    return exceptions;
}

/*
 * Each 8x8 block of the output (eight rows of eight lanes) comes from eight runs of eight
 * values, one per lane, and is transposed from them. Blocks of full lanes are loaded
 * directly; at the end of the lanes, missing values are loaded as zeros. Values above the
 * limit are cleared as they are loaded; the comparison competes with the transposition
 * for the same execution port, so it is left out when there is no limit.
 */
static void to_lane_order_avx512(const uint64_t* values, uint32_t count, uint64_t limit,
                                 uint64_t* out) {
    __m512i limit_vector = _mm512_set1_epi64((long long)limit);
    bool clear = limit != UINT64_MAX;
    uint32_t rows = sx_bitpack_rows(count);
    for (uint32_t first_lane = 0; first_lane < SX_BITPACK_LANES; first_lane += VECTOR_LANES) {
        for (uint32_t row = 0; row < rows; row += 8u) {
            bool full =
                row + 8u <= rows && (first_lane + VECTOR_LANES - 1u) * rows + row + 8u <= count;
            __m512i block[8];
            for (uint32_t j = 0; j < 8u; j++) {
                /* Values of lane first_lane + j in rows [row, row + 8). */
                uint32_t start = (first_lane + j) * rows + row;
                __mmask8 present = 0xff;
                if (!full) {
                    uint32_t in_lane = rows - row < 8u ? rows - row : 8u;
                    uint32_t available = start < count ? count - start : 0;
                    present = first_lanes(available < in_lane ? available : in_lane);
                }
                __m512i v = present == 0 ? _mm512_setzero_si512()
                                         : _mm512_maskz_loadu_epi64(present, values + start);
                if (clear) {
                    v = _mm512_maskz_mov_epi64(_mm512_cmple_epu64_mask(v, limit_vector), v);
                }
                block[j] = v;
            }
            transpose_8x8(block);
            for (uint32_t j = 0; j < 8u && row + j < rows; j++) {
                _mm512_storeu_si512(out + (size_t)(row + j) * SX_BITPACK_LANES + first_lane,
                                    block[j]);
            }
        }
    }
}

const sx_codec_kernels sx_kernels_avx512 = {
    .level = SX_SIMD_AVX512,
    .bitpack = bitpack_avx512,
    .bitunpack = bitunpack_avx512,
    .decode_for = decode_for_avx512,
    .decode_alp = decode_alp_avx512,
    .decode_deltas = decode_deltas_avx512,
    .rebuild_deltas = rebuild_deltas_avx512,
    .encode_alp = encode_alp_avx512,
    .summarize_ints = summarize_ints_avx512,
    .first_non_multiple = first_non_multiple_avx512,
    .divide_exact = divide_exact_avx512,
    .width_counts = width_counts_avx512,
    .find_exceptions = find_exceptions_avx512,
    .to_lane_order = to_lane_order_avx512,
};

#endif /* SX_ARCH_X86_64 */
