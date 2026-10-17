/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Scalar steps shared by the AVX2 and NEON ALP encoding kernels. Those kernels encode the
 * values in a loop that only records which values were kept, one byte per group of eight
 * (bit k of kept_bytes[g] is value 8g + k), and find the digit range in a second pass:
 * keeping the minimum and maximum in the first loop costs more there, since neither
 * instruction set has 64-bit integer min/max and compare-and-select chains are slow.
 */

#ifndef SX_ENCODING_KERNELS_ALP_H
#define SX_ENCODING_KERNELS_ALP_H

#include "common/bits.h"
#include "encoding/kernels.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Encodes values[begin, end) one by one, at most eight; returns their kept bits. */
static inline uint8_t sx_alp_encode_group(const double* values, uint32_t begin, uint32_t end,
                                          const sx_alp_scales* scales, int64_t* digits) {
    uint8_t kept = 0;
    for (uint32_t i = begin; i < end; i++) {
        if (sx_alp_encode_value(values[i], scales, &digits[i])) {
            kept |= (uint8_t)(1u << (i - begin));
        }
    }
    return kept;
}

/* Turns kept bytes into exception bits (see kernels.h) and returns the exception count. */
static inline uint32_t sx_alp_exceptions_from_kept(const uint8_t* kept_bytes, uint32_t count,
                                                   uint64_t* exception_bits) {
    uint32_t exceptions = 0;
    for (uint32_t word = 0; word * 64u < count; word++) {
        uint32_t values_in_word = count - word * 64u < 64u ? count - word * 64u : 64u;
        uint64_t kept = 0;
        memcpy(&kept, kept_bytes + (size_t)word * 8u, sx_div_round_up(values_in_word, 8));
        exception_bits[word] = ~kept & sx_low_mask_u64(values_in_word);
        exceptions += sx_popcount_u64(exception_bits[word]);
    }
    return exceptions;
}

/*
 * Widens `range` by the digits of values[begin, end) that are not exceptions. Exceptions
 * are replaced by neutral values instead of being skipped with a branch, since they come
 * at unpredictable positions; locals keep the range out of memory while it changes.
 */
static inline void sx_alp_range_add_kept(const int64_t* digits, uint32_t begin, uint32_t end,
                                         uint64_t exception_word, sx_alp_digit_range* range) {
    int64_t min = range->min;
    int64_t max = range->max;
    for (uint32_t i = begin; i < end; i++) {
        bool kept = ((exception_word >> (i - begin)) & 1u) == 0;
        int64_t low = kept ? digits[i] : INT64_MAX;
        int64_t high = kept ? digits[i] : INT64_MIN;
        min = low < min ? low : min;
        max = high > max ? high : max;
    }
    range->min = min;
    range->max = max;
}

#endif /* SX_ENCODING_KERNELS_ALP_H */
