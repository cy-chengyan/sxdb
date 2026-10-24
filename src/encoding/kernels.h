/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The hot loops of the column codecs ("kernels"), with one implementation per
 * instruction set. See docs/DESIGN.md section 4.2.
 *
 * Every kernel exists as portable C (kernels_scalar.c), which is the reference, and as
 * versions for the x86-64-v3 (AVX2) and x86-64-v4 (AVX-512) levels on x86_64 and a NEON
 * version on ARM64; where a vector version does not pay off, a set uses the scalar steps
 * of kernels_int.h. The best set the CPU supports is chosen on first use. Every
 * implementation produces exactly the same output as the portable one for every input,
 * corrupted payloads included; the unit tests check this for each set the machine
 * supports.
 *
 * Kernels trust their arguments: widths are at most 64, counts are 1 to SX_VECTOR_SIZE
 * unless a kernel says otherwise, and packed input holds sx_bitpack_size(count, width)
 * bytes. Codecs validate payloads before calling a kernel. Kernels that write whole rows
 * need room for sx_bitpack_padded_count(count) values in `out`.
 */

#ifndef SX_ENCODING_KERNELS_H
#define SX_ENCODING_KERNELS_H

#include "common/bits.h"
#include "common/compiler.h"
#include "encoding/bitpack.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum sx_simd_level {
    SX_SIMD_SCALAR = 0,
    SX_SIMD_AVX2 = 1,
    SX_SIMD_AVX512 = 2,
    SX_SIMD_NEON = 3,
} sx_simd_level;

#define SX_SIMD_LEVEL_COUNT 4u

/* The four powers of ten of one ALP exponent/factor pair (see float_codec.h). */
typedef struct sx_alp_scales {
    double encode_exponent; /* 10^exponent */
    double encode_factor;   /* 10^-factor */
    double decode_factor;   /* 10^factor */
    double decode_exponent; /* 10^-exponent */
} sx_alp_scales;

/* Smallest and largest digits of the values ALP encoded without exception. */
typedef struct sx_alp_digit_range {
    int64_t min; /* INT64_MAX if every value is an exception */
    int64_t max; /* INT64_MIN if every value is an exception */
} sx_alp_digit_range;

/*
 * Range of a chunk of integers and of its deltas (values[i + 1] - values[i], wrapping
 * modulo 2^64 and read as signed), which decide its encoding (see int_codec.c).
 */
typedef struct sx_int_summary {
    int64_t min;
    int64_t max;
    int64_t delta_min; /* 0 if there are no deltas */
    int64_t delta_max; /* 0 if there are no deltas */
    uint32_t changes;  /* number of deltas that are not zero */
} sx_int_summary;

/*
 * Division by a divisor that is known to divide every dividend exactly, and a test for
 * that, both with one shift and one multiplication instead of a slow division; see
 * sx_exact_divisor_make and sx_multiple_test_make below.
 */
typedef struct sx_exact_divisor {
    uint32_t shift;   /* factors of two of the divisor */
    uint64_t inverse; /* inverse of the odd part modulo 2^64 */
} sx_exact_divisor;

typedef struct sx_multiple_test {
    sx_exact_divisor exact;
    uint64_t low_bits;     /* bits that must be zero in a multiple */
    uint64_t max_quotient; /* largest quotient of a multiple */
} sx_multiple_test;

typedef struct sx_codec_kernels {
    sx_simd_level level;

    /*
     * Packs (values[i] - base) for i < count, cut to `width` bits, in the interleaved
     * layout of bitpack.h. Writes sx_bitpack_size(count, width) bytes.
     */
    void (*bitpack)(const uint64_t* values, uint32_t count, uint32_t width, uint64_t base,
                    uint8_t* out);

    /* Unpacks `count` values. Writes sx_bitpack_padded_count(count) values. */
    void (*bitunpack)(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t* out);

    /*
     * Frame-of-reference decoding: out[i] = base + unpacked[i] * multiplier, wrapping
     * modulo 2^64. Writes sx_bitpack_padded_count(count) values.
     */
    void (*decode_for)(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
                       uint64_t multiplier, uint64_t* out);

    /*
     * ALP decoding: out[i] = sx_alp_decode(base + unpacked[i], scales). Writes
     * sx_bitpack_padded_count(count) values.
     */
    void (*decode_alp)(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
                       const sx_alp_scales* scales, double* out);

    /*
     * First pass of DELTA_FOR and DELTA_PATCHED decoding: decodes the deltas
     * base + unpacked[i] * multiplier of `slot_count` slots (1 to SX_VECTOR_SIZE - 1) as
     * decode_for does, and stores row r of the block in row r + 1 of `scratch` (rows of
     * 16 values). Row 0 of `scratch` is set to zero, and so are the rows after the last
     * one up to sx_delta_scratch_last_row(rows). `lane_totals[l]` is set to the sum of
     * the deltas in lane l, padding included, wrapping modulo 2^64. `scratch` holds
     * SX_DELTA_SCRATCH_SIZE values.
     */
    void (*decode_deltas)(const uint8_t* packed, uint32_t slot_count, uint32_t width, uint64_t base,
                          uint64_t multiplier, uint64_t* scratch, uint64_t* lane_totals);

    /*
     * Second pass: rebuilds values from deltas in the lane-contiguous order of bitpack.h,
     * laid out in `scratch` as decode_deltas leaves them, with `lane_totals` the sums of
     * the lanes. With rows = ceil(slot_count / 16), lane l starts at `first` plus the
     * totals of the lanes before it, and out[l * rows + j] is that start plus scratch rows
     * 0 to j of lane l. So out[0] = first and out[i + 1] = out[i] + delta of slot i,
     * wrapping modulo 2^64. Writes the 16 * rows values of all lanes (those past
     * slot_count come from padding) and also out[16 * rows] when slot_count is
     * 16 * rows: max(slot_count + 1, 16 * rows) values, at most SX_VECTOR_SIZE.
     */
    void (*rebuild_deltas)(const uint64_t* scratch, uint32_t slot_count, uint64_t first,
                           const uint64_t* lane_totals, uint64_t* out);

    /*
     * ALP encoding of `count` values with one exponent/factor pair (sx_alp_encode_value).
     * Sets bit i of `exception_bits` (ceil(count / 64) words, unused bits cleared) for
     * every value that does not round-trip. digits[i] is set for every other value and
     * unspecified for exceptions. Returns the number of exceptions.
     */
    uint32_t (*encode_alp)(const double* values, uint32_t count, const sx_alp_scales* scales,
                           int64_t* digits, uint64_t* exception_bits, sx_alp_digit_range* range);

    /*
     * Fills `summary` for the `count` values and writes their count - 1 deltas to
     * `deltas`.
     */
    void (*summarize_ints)(const int64_t* values, uint32_t count, uint64_t* deltas,
                           sx_int_summary* summary);

    /*
     * Index of the first i in [start, count) for which numbers[i] - base (wrapping) is not
     * a multiple according to `test`, or `count` if there is none. Writes the quotients
     * sx_divide_exact(numbers[i] - base, &test->exact) of the multiples, i in [start,
     * result), to `quotients`, and may write anything to quotients[result, count). `start`
     * may be equal to `count`, and `count` may be 0.
     */
    uint32_t (*first_non_multiple)(const uint64_t* numbers, uint32_t start, uint32_t count,
                                   uint64_t base, const sx_multiple_test* test,
                                   uint64_t* quotients);

    /*
     * out[i] = sx_divide_exact(numbers[i] - base, divisor) for i < count, wrapping. `out`
     * may be `numbers`; `count` may be 0.
     */
    void (*divide_exact)(const uint64_t* numbers, uint32_t count, uint64_t base,
                         const sx_exact_divisor* divisor, uint64_t* out);

    /*
     * Counts the `count` numbers of each width, from max_width (at most 64; no number may
     * be wider) down: sets counts[w] to how many are exactly w bits wide, and stops at the
     * first w at which `enough` numbers are at least w bits wide, or at 0. Returns that
     * last width w; counts below it are not set.
     */
    uint32_t (*width_counts)(const uint64_t* numbers, uint32_t count, uint32_t max_width,
                             uint32_t enough, uint32_t* counts);

    /*
     * Sets bit i of `bits` (ceil(count / 64) words, unused bits cleared) for every offset
     * above `limit`: the exceptions of DELTA_PATCHED. Returns how many there are.
     */
    uint32_t (*find_exceptions)(const uint64_t* offsets, uint32_t count, uint64_t limit,
                                uint64_t* bits);

    /*
     * Reorders `count` values into the lane-contiguous order of bitpack.h: value i goes to
     * lane i / rows, row i % rows. Values above `limit` are written as 0 (UINT64_MAX keeps
     * all). Writes sx_bitpack_padded_count(count) values; the padding positions are zero.
     */
    void (*to_lane_order)(const uint64_t* values, uint32_t count, uint64_t limit, uint64_t* out);
} sx_codec_kernels;

/* The kernel set in use. The first call chooses the best set the CPU supports. */
const sx_codec_kernels* sx_codec_kernels_active(void);

/* True if this build has kernels for `level` and the CPU can run them. */
bool sx_simd_level_supported(sx_simd_level level);

/*
 * Switches all later codec calls to `level`, for tests and benchmarks. Must not be called
 * while other threads use the codecs. Returns false (and changes nothing) if the level is
 * not supported.
 */
bool sx_simd_set_level(sx_simd_level level);

const char* sx_simd_level_name(sx_simd_level level);

/* Parses a level name ("scalar", "avx2", "avx512", "neon"). */
bool sx_simd_level_from_name(const char* name, sx_simd_level* level);

/* The implementations, defined in kernels_*.c. Only kernels.c refers to them. */
extern const sx_codec_kernels sx_kernels_scalar;
extern const sx_codec_kernels sx_kernels_avx2;
extern const sx_codec_kernels sx_kernels_avx512;
extern const sx_codec_kernels sx_kernels_neon;

/* ---------------------------------------------------------------------------------------- */
/* Helpers shared by the kernels                                                            */
/* ---------------------------------------------------------------------------------------- */

/* How decoded offsets are multiplied. */
typedef enum sx_multiply_kind {
    SX_MULTIPLY_NONE,   /* multiplier 1, or width 0 (every offset is 0) */
    SX_MULTIPLY_NARROW, /* offsets and multiplier below 2^32: one 32x32->64-bit product */
    SX_MULTIPLY_WIDE,   /* full 64-bit products */
} sx_multiply_kind;

static inline sx_multiply_kind sx_multiply_kind_of(uint32_t width, uint64_t multiplier) {
    if (multiplier == 1 || width == 0) {
        return SX_MULTIPLY_NONE;
    }
    return width <= 32u && multiplier <= UINT32_MAX ? SX_MULTIPLY_NARROW : SX_MULTIPLY_WIDE;
}

/*
 * Scratch rows of the delta kernels: a zero row in front of at most SX_VECTOR_SIZE / 16
 * rows of deltas. rebuild_deltas reads blocks of up to eight rows, so the rows after the
 * last one are zeroed up to the end of its block.
 */
#define SX_DELTA_SCRATCH_SIZE ((SX_VECTOR_SIZE / SX_BITPACK_LANES + 1u) * SX_BITPACK_LANES)

static inline uint32_t sx_delta_scratch_last_row(uint32_t rows) {
    return (rows - 1u) | 7u;
}

/*
 * Lane starts for rebuild_deltas: `first` plus the totals of the lanes before each lane.
 * Returns the value after the last lane, which is out[16 * rows] of a full block.
 */
static inline uint64_t sx_delta_lane_starts(const uint64_t* lane_totals, uint64_t first,
                                            uint64_t* starts) {
    uint64_t start = first;
    for (uint32_t lane = 0; lane < SX_BITPACK_LANES; lane++) {
        starts[lane] = start;
        start += lane_totals[lane];
    }
    return start;
}

/* ---------------------------------------------------------------------------------------- */
/* ALP arithmetic shared by every kernel and by the codec                                   */
/* ---------------------------------------------------------------------------------------- */

/*
 * Adding 2^52 + 2^51 to a double x with |x| < 2^51 rounds it to the nearest integer n
 * (ties to even) and leaves n in the low mantissa bits: the bits of the sum are
 * SX_ALP_MAGIC_BITS + n. The same identity converts back from n to a double. Unlike a
 * conversion instruction, this works on any vector width (SSE2 and AVX2 have no 64-bit
 * integer to double conversion), so every kernel performs exactly the same operations.
 */
#define SX_ALP_MAGIC       6755399441055744.0
#define SX_ALP_MAGIC_BITS  UINT64_C(0x4338000000000000)
#define SX_ALP_ROUND_LIMIT 2251799813685248.0 /* 2^51 */

/* The integer n as a double; exact for |n| < 2^51, deterministic for any n. */
static inline double sx_alp_digits_to_double(uint64_t digits) {
    return sx_bits_to_double(digits + SX_ALP_MAGIC_BITS) - SX_ALP_MAGIC;
}

/* The ALP decoding formula: digits * 10^factor * 10^-exponent. */
static inline double sx_alp_decode(uint64_t digits, const sx_alp_scales* scales) {
    return sx_alp_digits_to_double(digits) * scales->decode_factor * scales->decode_exponent;
}

/*
 * Encodes one value: digits = round(value * 10^exponent * 10^-factor). Returns true if
 * the value is kept, that is, if |scaled| < 2^51 and decoding the digits reproduces the
 * value bit for bit; `digits` is set either way. The check uses the decoding formula
 * itself, so encoder and decoder can never disagree.
 */
static inline bool sx_alp_encode_value(double value, const sx_alp_scales* scales, int64_t* digits) {
    double scaled = value * scales->encode_exponent * scales->encode_factor;
    uint64_t candidate = sx_double_to_bits(scaled + SX_ALP_MAGIC) - SX_ALP_MAGIC_BITS;
    double decoded = sx_alp_decode(candidate, scales);
    *digits = (int64_t)candidate;
    bool in_range = scaled > -SX_ALP_ROUND_LIMIT && scaled < SX_ALP_ROUND_LIMIT;
    return in_range && sx_double_to_bits(decoded) == sx_double_to_bits(value);
}

/* ---------------------------------------------------------------------------------------- */
/* Exact division shared by every kernel and by the codec                                   */
/* ---------------------------------------------------------------------------------------- */

/*
 * For a divisor d = odd * 2^shift that divides the dividend exactly, shifting out the
 * factors of two and multiplying by the inverse of the odd part modulo 2^64 gives the
 * quotient. A divisor of 0 (all dividends are 0) divides by 1.
 */
static inline sx_exact_divisor sx_exact_divisor_make(uint64_t divisor) {
    sx_exact_divisor result = {.shift = 0, .inverse = 1};
    if (divisor == 0) {
        return result;
    }
    result.shift = sx_trailing_zeros_u64(divisor);
    uint64_t odd = divisor >> result.shift;
    if (odd == 1) {
        return result; /* a power of two: the shift alone */
    }
    /* (3 * odd) XOR 2 is the inverse modulo 2^5, and Newton's iteration doubles the
     * correct low bits each step: 10, 20, 40, 80. */
    uint64_t inverse = (3u * odd) ^ 2u;
    for (int step = 0; step < 4; step++) {
        inverse *= 2u - odd * inverse;
    }
    result.inverse = inverse;
    return result;
}

static inline uint64_t sx_divide_exact(uint64_t dividend, const sx_exact_divisor* divisor) {
    return (dividend >> divisor->shift) * divisor->inverse;
}

/*
 * x is a multiple of d = odd * 2^shift exactly when its low `shift` bits are zero and
 * (x >> shift) * inverse(odd) (mod 2^64) is at most (2^64 - 1) / odd. The only multiple
 * of 0 is 0: all bits must be zero and any quotient passes.
 */
static inline sx_multiple_test sx_multiple_test_make(uint64_t divisor) {
    sx_multiple_test test;
    test.exact = sx_exact_divisor_make(divisor);
    if (divisor == 0) {
        test.low_bits = UINT64_MAX;
        test.max_quotient = UINT64_MAX;
        return test;
    }
    test.low_bits = sx_low_mask_u64(test.exact.shift);
    uint64_t odd = divisor >> test.exact.shift;
    test.max_quotient = odd == 1 ? UINT64_MAX : UINT64_MAX / odd;
    return test;
}

/* Zero exactly when x is a multiple; computed without branches. */
static inline uint64_t sx_multiple_miss(uint64_t x, const sx_multiple_test* test) {
    uint64_t quotient = sx_divide_exact(x, &test->exact);
    return (x & test->low_bits) | (uint64_t)(quotient > test->max_quotient);
}

#endif /* SX_ENCODING_KERNELS_H */
