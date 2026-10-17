/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * AVX-512 codec kernels (x86_64, AVX-512 F and DQ). This file is compiled with
 * -mavx512f -mavx512dq and is only called after the CPU check in kernels.c. A row of 16
 * values is two 512-bit vectors of eight lanes. Every function computes exactly what its
 * portable counterpart in kernels_scalar.c computes; see kernels.h for the contracts.
 */

#include "encoding/bitpack.h"
#include "encoding/kernels.h"

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

static void decode_for_avx512(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
                              uint64_t multiplier, uint64_t* out) {
    __m512i mask = _mm512_set1_epi64((long long)sx_low_mask_u64(width));
    __m512i base_vector = _mm512_set1_epi64((long long)base);
    __m512i multiplier_vector = _mm512_set1_epi64((long long)multiplier);
    /* Offsets below 2^32 times a multiplier below 2^32 need one 32x32->64 product. */
    bool narrow = width <= 32u && multiplier <= UINT32_MAX;
    __m512i v[VECTORS_PER_ROW];
    for (uint32_t row = 0; row < sx_bitpack_rows(count); row++) {
        unpack_or_zero_row(packed, row, width, mask, v);
        uint64_t* dst = out + (size_t)row * SX_BITPACK_LANES;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            __m512i product;
            if (multiplier == 1) {
                product = v[q];
            } else if (narrow) {
                product = _mm512_mul_epu32(v[q], multiplier_vector);
            } else {
                product = _mm512_mullo_epi64(v[q], multiplier_vector);
            }
            _mm512_storeu_si512(dst + q * VECTOR_LANES, _mm512_add_epi64(product, base_vector));
        }
    }
}

static void decode_alp_avx512(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
                              const sx_alp_scales* scales, double* out) {
    __m512i mask = _mm512_set1_epi64((long long)sx_low_mask_u64(width));
    __m512i shifted_base = _mm512_set1_epi64((long long)(base + SX_ALP_MAGIC_BITS));
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

/* Stores the eight values of a lane block, or only those that belong to the lane. */
static inline void store_lane_block(uint64_t* dst, uint32_t lane_slots, uint32_t row,
                                    __m512i values) {
    if (lane_slots >= row + 8u) {
        _mm512_storeu_si512(dst, values);
    } else if (lane_slots > row) {
        __mmask8 valid = (__mmask8)((1u << (lane_slots - row)) - 1u);
        _mm512_mask_storeu_epi64(dst, valid, values);
    }
}

/*
 * Each lane holds a run of consecutive slots, so the lanes are summed independently:
 * running sums go down the rows, starting from each lane's first value, and each 8x8
 * block (eight rows of eight lanes) is transposed so that a lane's eight values become
 * one vector stored in place.
 */
static void rebuild_deltas_avx512(const uint64_t* deltas, uint32_t slot_count, uint64_t first,
                                  uint64_t* out) {
    uint32_t rows = sx_bitpack_rows(slot_count);
    /* Lane totals, with two sums per half so that the additions do not wait on each other. */
    __m512i even[VECTORS_PER_ROW] = {_mm512_setzero_si512(), _mm512_setzero_si512()};
    __m512i odd[VECTORS_PER_ROW] = {_mm512_setzero_si512(), _mm512_setzero_si512()};
    uint32_t row = 0;
    for (; row + 2u <= rows; row += 2u) {
        const uint64_t* src = deltas + (size_t)row * SX_BITPACK_LANES;
        for (uint32_t h = 0; h < VECTORS_PER_ROW; h++) {
            even[h] = _mm512_add_epi64(even[h], _mm512_loadu_si512(src + h * VECTOR_LANES));
            odd[h] = _mm512_add_epi64(
                odd[h], _mm512_loadu_si512(src + SX_BITPACK_LANES + h * VECTOR_LANES));
        }
    }
    if (row < rows) {
        const uint64_t* src = deltas + (size_t)row * SX_BITPACK_LANES;
        for (uint32_t h = 0; h < VECTORS_PER_ROW; h++) {
            even[h] = _mm512_add_epi64(even[h], _mm512_loadu_si512(src + h * VECTOR_LANES));
        }
    }
    uint64_t lane_totals[SX_BITPACK_LANES];
    for (uint32_t h = 0; h < VECTORS_PER_ROW; h++) {
        _mm512_storeu_si512(lane_totals + h * VECTOR_LANES, _mm512_add_epi64(even[h], odd[h]));
    }

    /* Lane l starts after all slots of the lanes before it; lanes fill up in order. */
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
    for (uint32_t h = 0; h < VECTORS_PER_ROW; h++) {
        uint32_t first_lane = h * VECTOR_LANES;
        __m512i running = _mm512_loadu_si512(lane_starts + first_lane);
        for (row = 0; row < rows; row += 8u) {
            uint32_t block_rows = rows - row < 8u ? rows - row : 8u;
            __m512i block[8];
            for (uint32_t j = 0; j < 8u; j++) {
                if (j < block_rows) {
                    const uint64_t* src = deltas + (size_t)(row + j) * SX_BITPACK_LANES;
                    running = _mm512_add_epi64(running, _mm512_loadu_si512(src + first_lane));
                }
                block[j] = running;
            }
            transpose_8x8(block);
            uint64_t* dst = values + (size_t)first_lane * rows + row;
            if (block_rows == 8u && first_lane + VECTOR_LANES <= full_lanes) {
                for (uint32_t i = 0; i < 8u; i++) {
                    _mm512_storeu_si512(dst + (size_t)i * rows, block[i]);
                }
            } else {
                for (uint32_t i = 0; i < 8u; i++) {
                    store_lane_block(dst + (size_t)i * rows, lane_slots[first_lane + i], row,
                                     block[i]);
                }
            }
        }
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

const sx_codec_kernels sx_kernels_avx512 = {
    .level = SX_SIMD_AVX512,
    .bitpack = bitpack_avx512,
    .bitunpack = bitunpack_avx512,
    .decode_for = decode_for_avx512,
    .decode_alp = decode_alp_avx512,
    .rebuild_deltas = rebuild_deltas_avx512,
    .encode_alp = encode_alp_avx512,
};

#endif /* SX_ARCH_X86_64 */
