/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The hot loops of the column codecs ("kernels"), with one implementation per
 * instruction set. See docs/DESIGN.md section 4.2.
 *
 * Every kernel exists as portable C (kernels_scalar.c), which is the reference, and as
 * AVX2 and AVX-512 versions on x86_64 and a NEON version on ARM64. The best set the CPU
 * supports is chosen on first use. Every implementation produces exactly the same output
 * as the portable one for every input, corrupted payloads included; the unit tests check
 * this for each set the machine supports.
 *
 * Kernels trust their arguments: widths are at most 64, counts are 1 to SX_VECTOR_SIZE,
 * and packed input holds sx_bitpack_size(count, width) bytes. Codecs validate payloads
 * before calling a kernel. Kernels that write whole rows need room for
 * sx_bitpack_padded_count(count) values in `out`.
 */

#ifndef SX_ENCODING_KERNELS_H
#define SX_ENCODING_KERNELS_H

#include "common/bits.h"

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
     * Rebuilds values from `slot_count` deltas stored in the lane-contiguous order of
     * bitpack.h: out[0] = first and out[i + 1] = out[i] + delta of slot i, wrapping
     * modulo 2^64. `deltas` holds sx_bitpack_padded_count(slot_count) entries. Writes
     * exactly slot_count + 1 values.
     */
    void (*rebuild_deltas)(const uint64_t* deltas, uint32_t slot_count, uint64_t first,
                           uint64_t* out);

    /*
     * ALP encoding of `count` values with one exponent/factor pair (sx_alp_encode_value).
     * Sets bit i of `exception_bits` (ceil(count / 64) words, unused bits cleared) for
     * every value that does not round-trip. digits[i] is set for every other value and
     * unspecified for exceptions. Returns the number of exceptions.
     */
    uint32_t (*encode_alp)(const double* values, uint32_t count, const sx_alp_scales* scales,
                           int64_t* digits, uint64_t* exception_bits, sx_alp_digit_range* range);
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

#endif /* SX_ENCODING_KERNELS_H */
