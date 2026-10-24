/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Scalar steps of the integer encoding kernels (see kernels.h), shared by the portable
 * kernels and by the SIMD kernels, which use them for the last few values of a chunk or
 * where a vector version does not pay off.
 */

#ifndef SX_ENCODING_KERNELS_INT_H
#define SX_ENCODING_KERNELS_INT_H

#include "common/bits.h"
#include "encoding/kernels.h"

#include <stdbool.h>
#include <stdint.h>

/* The summary of values[0] alone, with the first delta if there is one. */
static inline void sx_summary_start(const int64_t* values, uint32_t count,
                                    sx_int_summary* summary) {
    int64_t first_delta = count >= 2 ? (int64_t)((uint64_t)values[1] - (uint64_t)values[0]) : 0;
    summary->min = values[0];
    summary->max = values[0];
    summary->delta_min = first_delta;
    summary->delta_max = first_delta;
    summary->changes = 0;
}

static inline int64_t sx_min_i64(int64_t a, int64_t b) {
    return b < a ? b : a;
}

static inline int64_t sx_max_i64(int64_t a, int64_t b) {
    return b > a ? b : a;
}

/*
 * Adds values[begin, count), begin >= 1, to `summary` and writes their deltas to
 * deltas[begin - 1, count - 1). The deltas are written in a loop of their own: `deltas`
 * could alias `values` as far as the compiler knows, which would keep it from
 * vectorizing the loop that finds the ranges. That loop keeps the running values in
 * locals, for the same reason, and uses conditional moves for them.
 */
static inline void sx_summarize_ints_from(const int64_t* values, uint32_t begin, uint32_t count,
                                          uint64_t* deltas, sx_int_summary* summary) {
    int64_t min = summary->min;
    int64_t max = summary->max;
    int64_t delta_min = summary->delta_min;
    int64_t delta_max = summary->delta_max;
    uint32_t changes = summary->changes;
    for (uint32_t i = begin; i < count; i++) {
        int64_t delta = (int64_t)((uint64_t)values[i] - (uint64_t)values[i - 1]);
        min = sx_min_i64(min, values[i]);
        max = sx_max_i64(max, values[i]);
        delta_min = sx_min_i64(delta_min, delta);
        delta_max = sx_max_i64(delta_max, delta);
        changes += (uint32_t)(delta != 0);
    }
    summary->min = min;
    summary->max = max;
    summary->delta_min = delta_min;
    summary->delta_max = delta_max;
    summary->changes = changes;
    for (uint32_t i = begin; i < count; i++) {
        deltas[i - 1] = (uint64_t)values[i] - (uint64_t)values[i - 1];
    }
}

/*
 * Adds the widths of numbers[0, count) to counts[0, max_width]. Neighboring numbers
 * usually have the same width; four separate histograms keep the increments of one
 * counter from waiting on each other.
 */
static inline void sx_add_width_counts(const uint64_t* numbers, uint32_t count, uint32_t max_width,
                                       uint32_t* counts) {
    uint32_t counts0[65] = {0};
    uint32_t counts1[65] = {0};
    uint32_t counts2[65] = {0};
    uint32_t counts3[65] = {0};
    uint32_t i = 0;
    for (; i + 4u <= count; i += 4u) {
        counts0[sx_bit_width_u64(numbers[i])]++;
        counts1[sx_bit_width_u64(numbers[i + 1u])]++;
        counts2[sx_bit_width_u64(numbers[i + 2u])]++;
        counts3[sx_bit_width_u64(numbers[i + 3u])]++;
    }
    for (; i < count; i++) {
        counts0[sx_bit_width_u64(numbers[i])]++;
    }
    for (uint32_t width = 0; width <= max_width; width++) {
        counts[width] += counts0[width] + counts1[width] + counts2[width] + counts3[width];
    }
}

/*
 * split_exceptions for offsets[begin, count), after `exception_count` exceptions found
 * before `begin`; returns the new number of exceptions. Exceptions are rare, so the
 * branch for them is well predicted.
 */
static inline uint32_t sx_split_exceptions_from(const uint64_t* offsets, uint32_t begin,
                                                uint32_t count, uint64_t limit, uint64_t* packed,
                                                uint16_t* positions, uint64_t* exceptions,
                                                uint32_t exception_count) {
    for (uint32_t i = begin; i < count; i++) {
        uint64_t offset = offsets[i];
        packed[i] = offset;
        if (offset > limit) {
            packed[i] = 0;
            positions[exception_count] = (uint16_t)i;
            exceptions[exception_count] = offset;
            exception_count++;
        }
    }
    return exception_count;
}

/* first_non_multiple for numbers[start, count), one number at a time. */
static inline uint32_t sx_first_non_multiple_from(const uint64_t* numbers, uint32_t start,
                                                  uint32_t count, uint64_t base,
                                                  const sx_multiple_test* test,
                                                  uint64_t* quotients) {
    uint32_t i = start;
    for (; i < count; i++) {
        uint64_t x = numbers[i] - base;
        quotients[i] = sx_divide_exact(x, &test->exact);
        if (sx_multiple_miss(x, test) != 0) {
            break;
        }
    }
    return i;
}

#endif /* SX_ENCODING_KERNELS_INT_H */
