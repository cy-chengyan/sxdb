/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * AVX2 codec kernels (x86_64, the integer part of the x86-64-v3 level: AVX2, BMI1, BMI2,
 * LZCNT, and POPCNT). This file is compiled for those instruction sets only (see
 * src/CMakeLists.txt) and is only called after the CPU check in kernels.c. A row of 16
 * values is four 256-bit vectors of four lanes. Every function computes exactly what its
 * portable counterpart in kernels_scalar.c computes; see kernels.h for the contracts.
 */

#include "encoding/bitpack.h"
#include "encoding/kernels.h"
#include "encoding/kernels_alp.h"
#include "encoding/kernels_int.h"

#include "common/bits.h"
#include "common/compiler.h"

#include <string.h>

#if defined(SX_ARCH_X86_64)

#include <immintrin.h>

#define VECTORS_PER_ROW 4u
#define VECTOR_LANES    ((size_t)4)

static inline __m256i load(const void* p) {
    return _mm256_loadu_si256((const __m256i*)p);
}

static inline void store(void* p, __m256i v) {
    _mm256_storeu_si256((__m256i*)p, v);
}

/*
 * Unpacks row `row` of a block packed at `width` > 0 bits. The shifts use per-lane
 * counts (vpsrlvq), which unlike shifts by a register count do not compete with the
 * shuffles for the same execution port.
 */
static inline void unpack_row(const uint8_t* packed, uint32_t row, uint32_t width, __m256i mask,
                              __m256i* v) {
    uint64_t bit_position = (uint64_t)row * width;
    uint32_t offset = (uint32_t)(bit_position % 64u);
    const uint8_t* low = packed + (bit_position / 64u) * SX_BITPACK_ROW_BYTES;
    __m256i right = _mm256_set1_epi64x(offset);
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        v[q] = _mm256_srlv_epi64(load(low + q * sizeof(__m256i)), right);
    }
    if (offset + width > 64u) {
        /* The values continue in the next word of each lane. */
        __m256i left = _mm256_set1_epi64x(64u - offset);
        const uint8_t* high = low + SX_BITPACK_ROW_BYTES;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            v[q] = _mm256_or_si256(v[q], _mm256_sllv_epi64(load(high + q * sizeof(__m256i)), left));
        }
    }
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        v[q] = _mm256_and_si256(v[q], mask);
    }
}

static inline void unpack_or_zero_row(const uint8_t* packed, uint32_t row, uint32_t width,
                                      __m256i mask, __m256i* v) {
    if (width == 0) {
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            v[q] = _mm256_setzero_si256();
        }
    } else {
        unpack_row(packed, row, width, mask, v);
    }
}

/*
 * Low 64 bits of v * multiplier. AVX2 multiplies only 32-bit halves, so the product is
 * lo*lo + ((hi*lo + lo*hi) << 32).
 */
static inline __m256i multiply_u64(__m256i v, __m256i multiplier, __m256i multiplier_high) {
    __m256i v_high = _mm256_srli_epi64(v, 32);
    __m256i cross = _mm256_add_epi64(_mm256_mul_epu32(v_high, multiplier),
                                     _mm256_mul_epu32(v, multiplier_high));
    return _mm256_add_epi64(_mm256_mul_epu32(v, multiplier), _mm256_slli_epi64(cross, 32));
}

/*
 * Rows are packed in order, so each lane's current output word is kept in a register and
 * stored once it is full, instead of being read and written back for every row.
 */
static void bitpack_avx2(const uint64_t* values, uint32_t count, uint32_t width, uint64_t base,
                         uint8_t* out) {
    if (width == 0) {
        return; /* nothing is stored */
    }
    __m256i mask = _mm256_set1_epi64x((long long)sx_low_mask_u64(width));
    __m256i base_vector = _mm256_set1_epi64x((long long)base);
    __m256i words[VECTORS_PER_ROW];
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        words[q] = _mm256_setzero_si256();
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
        __m256i left = _mm256_set1_epi64x(offset);
        __m256i v[VECTORS_PER_ROW];
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            v[q] =
                _mm256_and_si256(_mm256_sub_epi64(load(src + q * VECTOR_LANES), base_vector), mask);
            words[q] = _mm256_or_si256(words[q], _mm256_sllv_epi64(v[q], left));
        }
        if (offset + width >= 64u) {
            /* The words are full; the next ones start with the bits that did not fit
             * (a shift by 64 yields zero). */
            __m256i right = _mm256_set1_epi64x(64u - offset);
            for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
                store(next_words + q * sizeof(__m256i), words[q]);
                words[q] = _mm256_srlv_epi64(v[q], right);
            }
            next_words += SX_BITPACK_ROW_BYTES;
        }
    }
    if (next_words < out + sx_bitpack_size(count, width)) {
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            store(next_words + q * sizeof(__m256i), words[q]);
        }
    }
}

static void bitunpack_avx2(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t* out) {
    __m256i mask = _mm256_set1_epi64x((long long)sx_low_mask_u64(width));
    __m256i v[VECTORS_PER_ROW];
    for (uint32_t row = 0; row < sx_bitpack_rows(count); row++) {
        unpack_or_zero_row(packed, row, width, mask, v);
        uint64_t* dst = out + (size_t)row * SX_BITPACK_LANES;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            store(dst + q * VECTOR_LANES, v[q]);
        }
    }
}

static void decode_for_avx2(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
                            uint64_t multiplier, uint64_t* out) {
    __m256i mask = _mm256_set1_epi64x((long long)sx_low_mask_u64(width));
    __m256i base_vector = _mm256_set1_epi64x((long long)base);
    __m256i multiplier_vector = _mm256_set1_epi64x((long long)multiplier);
    __m256i multiplier_high = _mm256_set1_epi64x((long long)(multiplier >> 32));
    /* Offsets below 2^32 times a multiplier below 2^32 need one 32x32->64 product. */
    bool narrow = width <= 32u && multiplier <= UINT32_MAX;
    __m256i v[VECTORS_PER_ROW];
    for (uint32_t row = 0; row < sx_bitpack_rows(count); row++) {
        unpack_or_zero_row(packed, row, width, mask, v);
        uint64_t* dst = out + (size_t)row * SX_BITPACK_LANES;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            __m256i product;
            if (multiplier == 1) {
                product = v[q];
            } else if (narrow) {
                product = _mm256_mul_epu32(v[q], multiplier_vector);
            } else {
                product = multiply_u64(v[q], multiplier_vector, multiplier_high);
            }
            store(dst + q * VECTOR_LANES, _mm256_add_epi64(product, base_vector));
        }
    }
}

static void decode_alp_avx2(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
                            const sx_alp_scales* scales, double* out) {
    __m256i mask = _mm256_set1_epi64x((long long)sx_low_mask_u64(width));
    __m256i shifted_base = _mm256_set1_epi64x((long long)(base + SX_ALP_MAGIC_BITS));
    __m256d magic = _mm256_set1_pd(SX_ALP_MAGIC);
    __m256d factor = _mm256_set1_pd(scales->decode_factor);
    __m256d exponent = _mm256_set1_pd(scales->decode_exponent);
    __m256i v[VECTORS_PER_ROW];
    for (uint32_t row = 0; row < sx_bitpack_rows(count); row++) {
        unpack_or_zero_row(packed, row, width, mask, v);
        double* dst = out + (size_t)row * SX_BITPACK_LANES;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            /* (base + offset) + MAGIC_BITS, read as a double, is digits + MAGIC. */
            __m256d x = _mm256_castsi256_pd(_mm256_add_epi64(v[q], shifted_base));
            x = _mm256_mul_pd(_mm256_mul_pd(_mm256_sub_pd(x, magic), factor), exponent);
            _mm256_storeu_pd(dst + q * VECTOR_LANES, x);
        }
    }
}

/* Transposes a 4x4 block of 64-bit values held in four vectors, in place. */
static inline void transpose_4x4(__m256i* m) {
    __m256i t0 = _mm256_unpacklo_epi64(m[0], m[1]);
    __m256i t1 = _mm256_unpackhi_epi64(m[0], m[1]);
    __m256i t2 = _mm256_unpacklo_epi64(m[2], m[3]);
    __m256i t3 = _mm256_unpackhi_epi64(m[2], m[3]);
    m[0] = _mm256_permute2x128_si256(t0, t2, 0x20);
    m[1] = _mm256_permute2x128_si256(t1, t3, 0x20);
    m[2] = _mm256_permute2x128_si256(t0, t2, 0x31);
    m[3] = _mm256_permute2x128_si256(t1, t3, 0x31);
}

/* Stores the four values of a lane block, or only those that belong to the lane. */
static inline void store_lane_block(uint64_t* dst, uint32_t lane_slots, uint32_t row,
                                    __m256i values) {
    if (lane_slots >= row + 4u) {
        store(dst, values);
    } else if (lane_slots > row) {
        __m256i valid = _mm256_cmpgt_epi64(_mm256_set1_epi64x((long long)(lane_slots - row)),
                                           _mm256_setr_epi64x(0, 1, 2, 3));
        _mm256_maskstore_epi64((long long*)dst, valid, values);
    }
}

/*
 * Each lane holds a run of consecutive slots, so the lanes are summed independently:
 * running sums go down the rows, starting from each lane's first value, and each 4x4
 * block (four rows of four lanes) is transposed so that a lane's four values become one
 * vector stored in place.
 */
static void rebuild_deltas_avx2(const uint64_t* deltas, uint32_t slot_count, uint64_t first,
                                uint64_t* out) {
    uint32_t rows = sx_bitpack_rows(slot_count);
    __m256i totals[VECTORS_PER_ROW];
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        totals[q] = _mm256_setzero_si256();
    }
    for (uint32_t row = 0; row < rows; row++) {
        const uint64_t* src = deltas + (size_t)row * SX_BITPACK_LANES;
        for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
            totals[q] = _mm256_add_epi64(totals[q], load(src + q * VECTOR_LANES));
        }
    }
    uint64_t lane_totals[SX_BITPACK_LANES];
    for (uint32_t q = 0; q < VECTORS_PER_ROW; q++) {
        store(lane_totals + q * VECTOR_LANES, totals[q]);
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
    for (uint32_t group = 0; group < VECTORS_PER_ROW; group++) {
        uint32_t first_lane = group * VECTOR_LANES;
        __m256i running = load(lane_starts + first_lane);
        for (uint32_t row = 0; row < rows; row += 4u) {
            uint32_t block_rows = rows - row < 4u ? rows - row : 4u;
            __m256i block[4];
            for (uint32_t j = 0; j < 4u; j++) {
                if (j < block_rows) {
                    const uint64_t* src = deltas + (size_t)(row + j) * SX_BITPACK_LANES;
                    running = _mm256_add_epi64(running, load(src + first_lane));
                }
                block[j] = running;
            }
            transpose_4x4(block);
            uint64_t* dst = values + (size_t)first_lane * rows + row;
            if (block_rows == 4u && first_lane + VECTOR_LANES <= full_lanes) {
                for (uint32_t i = 0; i < 4u; i++) {
                    store(dst + (size_t)i * rows, block[i]);
                }
            } else {
                for (uint32_t i = 0; i < 4u; i++) {
                    store_lane_block(dst + (size_t)i * rows, lane_slots[first_lane + i], row,
                                     block[i]);
                }
            }
        }
    }
}

/* Lane-wise minimum and maximum of 64-bit integers (AVX2 has no instruction for them). */
static inline __m256i min_i64x4(__m256i a, __m256i b) {
    return _mm256_blendv_epi8(a, b, _mm256_cmpgt_epi64(a, b));
}

static inline __m256i max_i64x4(__m256i a, __m256i b) {
    return _mm256_blendv_epi8(a, b, _mm256_cmpgt_epi64(b, a));
}

typedef struct alp_constants {
    __m256d encode_exponent;
    __m256d encode_factor;
    __m256d decode_factor;
    __m256d decode_exponent;
    __m256d magic;
    __m256i magic_bits;
    __m256d upper;
    __m256d lower;
} alp_constants;

/* Encodes four values; `kept` is all ones in the lanes that round-trip. */
static inline __m256i encode_four(const double* values, const alp_constants* c, __m256i* kept) {
    __m256d value = _mm256_loadu_pd(values);
    __m256d scaled = _mm256_mul_pd(_mm256_mul_pd(value, c->encode_exponent), c->encode_factor);
    __m256d shifted = _mm256_add_pd(scaled, c->magic);
    __m256d decoded = _mm256_mul_pd(
        _mm256_mul_pd(_mm256_sub_pd(shifted, c->magic), c->decode_factor), c->decode_exponent);
    __m256d in_range = _mm256_and_pd(_mm256_cmp_pd(scaled, c->lower, _CMP_GT_OQ),
                                     _mm256_cmp_pd(scaled, c->upper, _CMP_LT_OQ));
    __m256i exact = _mm256_cmpeq_epi64(_mm256_castpd_si256(decoded), _mm256_castpd_si256(value));
    *kept = _mm256_and_si256(_mm256_castpd_si256(in_range), exact);
    return _mm256_sub_epi64(_mm256_castpd_si256(shifted), c->magic_bits);
}

/* Kept bits of four lanes (bit k set where lane k is kept). */
static inline uint32_t kept_nibble(__m256i kept) {
    return (uint32_t)_mm256_movemask_pd(_mm256_castsi256_pd(kept));
}

/*
 * The digit range in a second pass (see kernels_alp.h): the values of a word without
 * exceptions are scanned with four independent minimum and maximum vectors, the values of
 * other words one by one.
 */
static void digit_range_avx2(const int64_t* digits, uint32_t count, const uint64_t* exception_bits,
                             sx_alp_digit_range* range) {
    __m256i min[4];
    __m256i max[4];
    for (uint32_t k = 0; k < 4u; k++) {
        min[k] = _mm256_set1_epi64x(INT64_MAX);
        max[k] = _mm256_set1_epi64x(INT64_MIN);
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
        for (; i + 4u * VECTOR_LANES <= end; i += 4u * VECTOR_LANES) {
            for (uint32_t k = 0; k < 4u; k++) {
                __m256i v = load(digits + i + k * VECTOR_LANES);
                min[k] = min_i64x4(min[k], v);
                max[k] = max_i64x4(max[k], v);
            }
        }
        sx_alp_range_add_kept(digits, i, end, 0, range);
    }
    int64_t lanes_min[VECTOR_LANES];
    int64_t lanes_max[VECTOR_LANES];
    store(lanes_min, min_i64x4(min_i64x4(min[0], min[1]), min_i64x4(min[2], min[3])));
    store(lanes_max, max_i64x4(max_i64x4(max[0], max[1]), max_i64x4(max[2], max[3])));
    for (uint32_t lane = 0; lane < VECTOR_LANES; lane++) {
        if (lanes_min[lane] < range->min) {
            range->min = lanes_min[lane];
        }
        if (lanes_max[lane] > range->max) {
            range->max = lanes_max[lane];
        }
    }
}

static uint32_t encode_alp_avx2(const double* values, uint32_t count, const sx_alp_scales* scales,
                                int64_t* digits, uint64_t* exception_bits,
                                sx_alp_digit_range* range) {
    alp_constants c = {
        .encode_exponent = _mm256_set1_pd(scales->encode_exponent),
        .encode_factor = _mm256_set1_pd(scales->encode_factor),
        .decode_factor = _mm256_set1_pd(scales->decode_factor),
        .decode_exponent = _mm256_set1_pd(scales->decode_exponent),
        .magic = _mm256_set1_pd(SX_ALP_MAGIC),
        .magic_bits = _mm256_set1_epi64x((long long)SX_ALP_MAGIC_BITS),
        .upper = _mm256_set1_pd(SX_ALP_ROUND_LIMIT),
        .lower = _mm256_set1_pd(-SX_ALP_ROUND_LIMIT),
    };
    uint8_t kept_bytes[SX_VECTOR_SIZE / 8];
    uint32_t i = 0;
    for (; i + 2u * VECTOR_LANES <= count; i += 2u * VECTOR_LANES) {
        __m256i kept0;
        __m256i kept1;
        store(digits + i, encode_four(values + i, &c, &kept0));
        store(digits + i + VECTOR_LANES, encode_four(values + i + VECTOR_LANES, &c, &kept1));
        kept_bytes[i / 8u] = (uint8_t)(kept_nibble(kept0) | kept_nibble(kept1) << VECTOR_LANES);
    }
    if (i < count) {
        kept_bytes[i / 8u] = sx_alp_encode_group(values, i, count, scales, digits);
    }
    uint32_t exceptions = sx_alp_exceptions_from_kept(kept_bytes, count, exception_bits);
    digit_range_avx2(digits, count, exception_bits, range);
    return exceptions;
}

/* The running minimums and maximums of summarize_ints, for one vector of four lanes. */
typedef struct int_ranges {
    __m256i min;
    __m256i max;
    __m256i delta_min;
    __m256i delta_max;
} int_ranges;

/* Adds the four values at values + i, and their deltas, to the ranges; returns how many
 * deltas are zero. */
static inline uint32_t add_to_ranges(const int64_t* values, uint32_t i, uint64_t* deltas,
                                     int_ranges* r) {
    __m256i v = load(values + i);
    __m256i previous = load(values + i - 1);
    __m256i delta = _mm256_sub_epi64(v, previous);
    store(deltas + i - 1, delta);
    r->min = min_i64x4(r->min, v);
    r->max = max_i64x4(r->max, v);
    r->delta_min = min_i64x4(r->delta_min, delta);
    r->delta_max = max_i64x4(r->delta_max, delta);
    __m256i same = _mm256_cmpeq_epi64(v, previous);
    return sx_popcount_u64((uint32_t)_mm256_movemask_pd(_mm256_castsi256_pd(same)));
}

/*
 * Each vector of values is subtracted from the same values loaded one element earlier,
 * which gives four deltas. Two sets of ranges take turns, since each compare-and-select
 * waits for the previous one; the last values are summarized one by one.
 */
static void summarize_ints_avx2(const int64_t* values, uint32_t count, uint64_t* deltas,
                                sx_int_summary* summary) {
    sx_summary_start(values, count, summary);
    int_ranges even = {
        .min = _mm256_set1_epi64x(summary->min),
        .max = _mm256_set1_epi64x(summary->max),
        .delta_min = _mm256_set1_epi64x(summary->delta_min),
        .delta_max = _mm256_set1_epi64x(summary->delta_max),
    };
    int_ranges odd = even;
    uint32_t equal = 0;
    uint32_t i = 1;
    for (; i + 2u * VECTOR_LANES <= count; i += 2u * VECTOR_LANES) {
        equal += add_to_ranges(values, i, deltas, &even);
        equal += add_to_ranges(values, i + VECTOR_LANES, deltas, &odd);
    }
    if (i + VECTOR_LANES <= count) {
        equal += add_to_ranges(values, i, deltas, &even);
        i += VECTOR_LANES;
    }
    int64_t lanes[4][VECTOR_LANES];
    store(lanes[0], min_i64x4(even.min, odd.min));
    store(lanes[1], max_i64x4(even.max, odd.max));
    store(lanes[2], min_i64x4(even.delta_min, odd.delta_min));
    store(lanes[3], max_i64x4(even.delta_max, odd.delta_max));
    for (uint32_t lane = 0; lane < VECTOR_LANES; lane++) {
        summary->min = sx_min_i64(summary->min, lanes[0][lane]);
        summary->max = sx_max_i64(summary->max, lanes[1][lane]);
        summary->delta_min = sx_min_i64(summary->delta_min, lanes[2][lane]);
        summary->delta_max = sx_max_i64(summary->delta_max, lanes[3][lane]);
    }
    summary->changes = (i - 1u) - equal;
    sx_summarize_ints_from(values, i, count, deltas, summary);
}

/* The constants of a multiple test, in vectors. Unsigned comparisons flip the sign bits. */
typedef struct multiple_vectors {
    __m256i base;
    __m256i shift;
    __m256i inverse;
    __m256i inverse_high;
    __m256i low_bits;
    __m256i sign;
    __m256i max_quotient; /* sign flipped */
    bool multiply;        /* false if the odd part of the divisor is 1: only low bits matter */
} multiple_vectors;

/*
 * True if a number of the eight at `numbers`, minus the base, is not a multiple; writes
 * the quotients of all eight.
 */
static inline bool any_misses(const uint64_t* numbers, const multiple_vectors* t,
                              uint64_t* quotients) {
    __m256i x[2] = {
        _mm256_sub_epi64(load(numbers), t->base),
        _mm256_sub_epi64(load(numbers + VECTOR_LANES), t->base),
    };
    __m256i quotient[2] = {
        _mm256_srlv_epi64(x[0], t->shift),
        _mm256_srlv_epi64(x[1], t->shift),
    };
    __m256i misses =
        _mm256_or_si256(_mm256_and_si256(x[0], t->low_bits), _mm256_and_si256(x[1], t->low_bits));
    /* Zero is a multiple of every divisor; blocks of zeros skip the multiplications. */
    __m256i any = _mm256_or_si256(x[0], x[1]);
    if (t->multiply && !_mm256_testz_si256(any, any)) {
        for (uint32_t k = 0; k < 2u; k++) {
            quotient[k] = multiply_u64(quotient[k], t->inverse, t->inverse_high);
            __m256i flipped = _mm256_xor_si256(quotient[k], t->sign);
            misses = _mm256_or_si256(misses, _mm256_cmpgt_epi64(flipped, t->max_quotient));
        }
    }
    store(quotients, quotient[0]);
    store(quotients + VECTOR_LANES, quotient[1]);
    return !_mm256_testz_si256(misses, misses);
}

/* Blocks of eight numbers are tested with one branch; the rest one by one. */
static uint32_t first_non_multiple_avx2(const uint64_t* numbers, uint32_t start, uint32_t count,
                                        uint64_t base, const sx_multiple_test* test,
                                        uint64_t* quotients) {
    multiple_vectors t = {
        .base = _mm256_set1_epi64x((long long)base),
        .shift = _mm256_set1_epi64x(test->exact.shift),
        .inverse = _mm256_set1_epi64x((long long)test->exact.inverse),
        .inverse_high = _mm256_set1_epi64x((long long)(test->exact.inverse >> 32)),
        .low_bits = _mm256_set1_epi64x((long long)test->low_bits),
        .sign = _mm256_set1_epi64x(INT64_MIN),
        .max_quotient = _mm256_set1_epi64x((long long)(test->max_quotient ^ (uint64_t)INT64_MIN)),
        .multiply = test->max_quotient != UINT64_MAX,
    };
    uint32_t i = start;
    for (; i + 2u * VECTOR_LANES <= count; i += 2u * VECTOR_LANES) {
        if (any_misses(numbers + i, &t, quotients + i)) {
            break;
        }
    }
    return sx_first_non_multiple_from(numbers, i, count, base, test, quotients);
}

static void divide_exact_avx2(const uint64_t* numbers, uint32_t count, uint64_t base,
                              const sx_exact_divisor* divisor, uint64_t* out) {
    __m256i base_vector = _mm256_set1_epi64x((long long)base);
    __m256i shift = _mm256_set1_epi64x(divisor->shift);
    __m256i inverse = _mm256_set1_epi64x((long long)divisor->inverse);
    __m256i inverse_high = _mm256_set1_epi64x((long long)(divisor->inverse >> 32));
    bool multiply = divisor->inverse != 1;
    uint32_t i = 0;
    for (; i + VECTOR_LANES <= count; i += VECTOR_LANES) {
        __m256i quotient =
            _mm256_srlv_epi64(_mm256_sub_epi64(load(numbers + i), base_vector), shift);
        if (multiply) {
            quotient = multiply_u64(quotient, inverse, inverse_high);
        }
        store(out + i, quotient);
    }
    for (; i < count; i++) {
        out[i] = sx_divide_exact(numbers[i] - base, divisor);
    }
}

/*
 * The widths of 32 numbers below 2^52, merged into the 32 bytes of one vector: one byte
 * per number, in some order. AVX2 has no leading zero count of 64-bit lanes, but such
 * numbers convert to doubles exactly: placed in the mantissa of 2^52, minus 2^52. The
 * exponent field of the result is 1022 + width for a non-zero number and 0 for zero.
 */
static inline __m256i merged_widths(const uint64_t* numbers) {
    __m256i magic_bits = _mm256_set1_epi64x(0x4330000000000000);
    __m256d magic = _mm256_set1_pd(4503599627370496.0); /* 2^52 */
    __m256i exponent_bias = _mm256_set1_epi64x(1022);
    __m256i zero = _mm256_setzero_si256();
    __m256i widths = zero;
    for (uint32_t k = 0; k < 8u; k++) {
        __m256i x = load(numbers + k * VECTOR_LANES);
        __m256d exact = _mm256_sub_pd(_mm256_castsi256_pd(_mm256_or_si256(x, magic_bits)), magic);
        __m256i exponent = _mm256_srli_epi64(_mm256_castpd_si256(exact), 52);
        /* Zero gives -1022; a 32-bit maximum with 0 clears both halves of it. */
        __m256i width = _mm256_max_epi32(_mm256_sub_epi64(exponent, exponent_bias), zero);
        widths = _mm256_or_si256(widths, _mm256_sllv_epi64(width, _mm256_set1_epi64x(8 * k)));
    }
    return widths;
}

/*
 * Blocks of 32 numbers are merged (merged_widths), and then the bytes of all blocks are
 * counted for each width. Wider numbers and the last few numbers are counted one by one.
 */
static void width_counts_avx2(const uint64_t* numbers, uint32_t count, uint32_t max_width,
                              uint32_t* counts) {
    memset(counts, 0, ((size_t)max_width + 1u) * sizeof(uint32_t));
    uint32_t block_count = 0;
    if (max_width <= 52u) {
        __m256i blocks[SX_VECTOR_SIZE / 32u];
        for (; (block_count + 1u) * 32u <= count; block_count++) {
            blocks[block_count] = merged_widths(numbers + block_count * 32u);
        }
        __m256i target = _mm256_setzero_si256();
        __m256i one = _mm256_set1_epi8(1);
        for (uint32_t width = 0; width <= max_width; width++) {
            uint32_t total = 0;
            for (uint32_t b = 0; b < block_count; b++) {
                __m256i same = _mm256_cmpeq_epi8(blocks[b], target);
                total += sx_popcount_u64((uint32_t)_mm256_movemask_epi8(same));
            }
            counts[width] = total;
            target = _mm256_add_epi8(target, one);
        }
    }
    uint32_t counted = block_count * 32u;
    sx_add_width_counts(numbers + counted, count - counted, max_width, counts);
}

static uint32_t split_exceptions_avx2(const uint64_t* offsets, uint32_t count, uint64_t limit,
                                      uint64_t* packed, uint16_t* positions, uint64_t* exceptions) {
    __m256i sign = _mm256_set1_epi64x(INT64_MIN);
    __m256i limit_vector = _mm256_set1_epi64x((long long)(limit ^ (uint64_t)INT64_MIN));
    uint32_t exception_count = 0;
    uint32_t i = 0;
    for (; i + VECTOR_LANES <= count; i += VECTOR_LANES) {
        __m256i x = load(offsets + i);
        __m256i wide = _mm256_cmpgt_epi64(_mm256_xor_si256(x, sign), limit_vector);
        store(packed + i, _mm256_andnot_si256(wide, x));
        uint32_t bits = (uint32_t)_mm256_movemask_pd(_mm256_castsi256_pd(wide));
        for (; bits != 0; bits &= bits - 1u) {
            uint32_t index = i + sx_trailing_zeros_u64(bits);
            positions[exception_count] = (uint16_t)index;
            exceptions[exception_count] = offsets[index];
            exception_count++;
        }
    }
    return sx_split_exceptions_from(offsets, i, count, limit, packed, positions, exceptions,
                                    exception_count);
}

/*
 * Each 4x4 block of the output (four rows of four lanes) comes from four runs of four
 * values, one per lane, and is transposed from them. Blocks of full lanes are loaded
 * directly; at the end of the lanes, missing values are loaded as zeros.
 */
static void to_lane_order_avx2(const uint64_t* values, uint32_t count, uint64_t* out) {
    uint32_t rows = sx_bitpack_rows(count);
    __m256i lane_index = _mm256_setr_epi64x(0, 1, 2, 3);
    for (uint32_t first_lane = 0; first_lane < SX_BITPACK_LANES; first_lane += VECTOR_LANES) {
        for (uint32_t row = 0; row < rows; row += 4u) {
            bool full =
                row + 4u <= rows && (first_lane + VECTOR_LANES - 1u) * rows + row + 4u <= count;
            __m256i block[4];
            for (uint32_t j = 0; j < 4u; j++) {
                /* Values of lane first_lane + j in rows [row, row + 4). */
                uint32_t start = (first_lane + j) * rows + row;
                if (full) {
                    block[j] = load(values + start);
                    continue;
                }
                uint32_t in_lane = rows - row < 4u ? rows - row : 4u;
                uint32_t available = start < count ? count - start : 0;
                uint32_t present = available < in_lane ? available : in_lane;
                if (present == 0) {
                    block[j] = _mm256_setzero_si256();
                } else {
                    __m256i mask = _mm256_cmpgt_epi64(_mm256_set1_epi64x(present), lane_index);
                    block[j] = _mm256_maskload_epi64((const long long*)(values + start), mask);
                }
            }
            transpose_4x4(block);
            for (uint32_t j = 0; j < 4u && row + j < rows; j++) {
                store(out + (size_t)(row + j) * SX_BITPACK_LANES + first_lane, block[j]);
            }
        }
    }
}

const sx_codec_kernels sx_kernels_avx2 = {
    .level = SX_SIMD_AVX2,
    .bitpack = bitpack_avx2,
    .bitunpack = bitunpack_avx2,
    .decode_for = decode_for_avx2,
    .decode_alp = decode_alp_avx2,
    .rebuild_deltas = rebuild_deltas_avx2,
    .encode_alp = encode_alp_avx2,
    .summarize_ints = summarize_ints_avx2,
    .first_non_multiple = first_non_multiple_avx2,
    .divide_exact = divide_exact_avx2,
    .width_counts = width_counts_avx2,
    .split_exceptions = split_exceptions_avx2,
    .to_lane_order = to_lane_order_avx2,
};

#endif /* SX_ARCH_X86_64 */
