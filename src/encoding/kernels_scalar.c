/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Portable codec kernels: the reference implementation of kernels.h. They are written
 * for clarity; the SIMD kernels must produce exactly the same output.
 */

#include "encoding/bitpack.h"
#include "encoding/kernels.h"
#include "encoding/kernels_int.h"

#include "common/bits.h"
#include "common/compiler.h"

#include <string.h>

/* ORs `bits` into word `lane` of the row of words starting at `row_words`. */
static void or_word(uint8_t* row_words, uint32_t lane, uint64_t bits) {
    uint8_t* word = row_words + lane * sizeof(uint64_t);
    sx_store_u64(word, sx_load_u64(word) | bits);
}

static void bitpack_scalar(const uint64_t* values, uint32_t count, uint32_t width, uint64_t base,
                           uint8_t* out) {
    memset(out, 0, sx_bitpack_size(count, width));
    if (width == 0) {
        return;
    }
    uint64_t mask = sx_low_mask_u64(width);
    uint32_t rows = sx_bitpack_rows(count);
    for (uint32_t row = 0; row < rows; row++) {
        uint64_t bit_position = (uint64_t)row * width;
        uint32_t offset = (uint32_t)(bit_position % 64u);
        uint8_t* low_words = out + (bit_position / 64u) * SX_BITPACK_ROW_BYTES;
        for (uint32_t lane = 0; lane < SX_BITPACK_LANES; lane++) {
            uint32_t index = row * SX_BITPACK_LANES + lane;
            uint64_t value = index < count ? (values[index] - base) & mask : 0;
            or_word(low_words, lane, value << offset);
            if (offset + width > 64u) {
                /* The value continues in the next word of the lane. */
                or_word(low_words + SX_BITPACK_ROW_BYTES, lane, value >> (64u - offset));
            }
        }
    }
}

/* Value `lane` of row `row` of a block packed at `width` > 0 bits. */
static uint64_t unpack_value(const uint8_t* packed, uint32_t row, uint32_t lane, uint32_t width) {
    uint64_t bit_position = (uint64_t)row * width;
    uint32_t offset = (uint32_t)(bit_position % 64u);
    const uint8_t* low =
        packed + (bit_position / 64u) * SX_BITPACK_ROW_BYTES + lane * sizeof(uint64_t);
    uint64_t value = sx_load_u64(low) >> offset;
    if (offset + width > 64u) {
        value |= sx_load_u64(low + SX_BITPACK_ROW_BYTES) << (64u - offset);
    }
    return value & sx_low_mask_u64(width);
}

static void bitunpack_scalar(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t* out) {
    uint32_t rows = sx_bitpack_rows(count);
    if (width == 0) {
        memset(out, 0, (size_t)rows * SX_BITPACK_ROW_BYTES);
        return;
    }
    for (uint32_t row = 0; row < rows; row++) {
        for (uint32_t lane = 0; lane < SX_BITPACK_LANES; lane++) {
            out[(size_t)row * SX_BITPACK_LANES + lane] = unpack_value(packed, row, lane, width);
        }
    }
}

static void decode_for_scalar(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
                              uint64_t multiplier, uint64_t* out) {
    bitunpack_scalar(packed, count, width, out);
    for (uint32_t i = 0; i < sx_bitpack_padded_count(count); i++) {
        out[i] = base + out[i] * multiplier;
    }
}

static void decode_alp_scalar(const uint8_t* packed, uint32_t count, uint32_t width, uint64_t base,
                              const sx_alp_scales* scales, double* out) {
    uint64_t offsets[SX_VECTOR_SIZE];
    bitunpack_scalar(packed, count, width, offsets);
    for (uint32_t i = 0; i < sx_bitpack_padded_count(count); i++) {
        out[i] = sx_alp_decode(base + offsets[i], scales);
    }
}

static void decode_deltas_scalar(const uint8_t* packed, uint32_t slot_count, uint32_t width,
                                 uint64_t base, uint64_t multiplier, uint64_t* scratch,
                                 uint64_t* lane_totals) {
    uint32_t rows = sx_bitpack_rows(slot_count);
    uint64_t totals[SX_BITPACK_LANES] = {0};
    memset(scratch, 0, SX_BITPACK_ROW_BYTES);
    for (uint32_t row = 0; row < rows; row++) {
        uint64_t* dst = scratch + (size_t)(row + 1u) * SX_BITPACK_LANES;
        for (uint32_t lane = 0; lane < SX_BITPACK_LANES; lane++) {
            uint64_t offset = width == 0 ? 0 : unpack_value(packed, row, lane, width);
            dst[lane] = base + offset * multiplier;
            totals[lane] += dst[lane];
        }
    }
    uint32_t last_row = sx_delta_scratch_last_row(rows);
    if (last_row > rows) {
        memset(scratch + (size_t)(rows + 1u) * SX_BITPACK_LANES, 0,
               (size_t)(last_row - rows) * SX_BITPACK_ROW_BYTES);
    }
    memcpy(lane_totals, totals, sizeof(totals));
}

static void rebuild_deltas_scalar(const uint64_t* scratch, uint32_t slot_count, uint64_t first,
                                  const uint64_t* lane_totals, uint64_t* out) {
    uint32_t rows = sx_bitpack_rows(slot_count);
    uint64_t starts[SX_BITPACK_LANES];
    uint64_t end = sx_delta_lane_starts(lane_totals, first, starts);
    /* Lane by lane, down the rows; scratch row 0 is zero, so out[lane * rows] is the start. */
    for (uint32_t lane = 0; lane < SX_BITPACK_LANES; lane++) {
        uint64_t value = starts[lane];
        for (uint32_t j = 0; j < rows; j++) {
            value += scratch[(size_t)j * SX_BITPACK_LANES + lane];
            out[(size_t)lane * rows + j] = value;
        }
    }
    if (slot_count == SX_BITPACK_LANES * rows) {
        out[(size_t)SX_BITPACK_LANES * rows] = end;
    }
}

static void fill_arithmetic_scalar(void* out, uint32_t count, uint64_t first, uint64_t step) {
    uint8_t* dst = out;
    uint64_t value = first;
    for (uint32_t i = 0; i < count; i++) {
        sx_store_u64(dst + (size_t)i * sizeof(uint64_t), value);
        value += step;
    }
}

static uint32_t encode_alp_scalar(const double* values, uint32_t count, const sx_alp_scales* scales,
                                  int64_t* digits, uint64_t* exception_bits,
                                  sx_alp_digit_range* range) {
    memset(exception_bits, 0, sx_div_round_up(count, 64) * sizeof(uint64_t));
    range->min = INT64_MAX;
    range->max = INT64_MIN;
    uint32_t exceptions = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!sx_alp_encode_value(values[i], scales, &digits[i])) {
            exception_bits[i / 64u] |= UINT64_C(1) << (i % 64u);
            exceptions++;
            continue;
        }
        if (digits[i] < range->min) {
            range->min = digits[i];
        }
        if (digits[i] > range->max) {
            range->max = digits[i];
        }
    }
    return exceptions;
}

static void summarize_ints_scalar(const int64_t* values, uint32_t count, uint64_t* deltas,
                                  sx_int_summary* summary) {
    sx_summary_start(values, count, summary);
    sx_summarize_ints_from(values, 1, count, deltas, summary);
}

/* Blocks of 16 numbers are tested with one branch, since nearly all numbers are multiples. */
static uint32_t first_non_multiple_scalar(const uint64_t* numbers, uint32_t start, uint32_t count,
                                          uint64_t base, const sx_multiple_test* test,
                                          uint64_t* quotients) {
    uint32_t i = start;
    while (i + 16u <= count) {
        uint64_t misses = 0;
        for (uint32_t j = 0; j < 16u; j++) {
            uint64_t x = numbers[i + j] - base;
            quotients[i + j] = sx_divide_exact(x, &test->exact);
            misses |= sx_multiple_miss(x, test);
        }
        if (misses != 0) {
            break;
        }
        i += 16u;
    }
    return sx_first_non_multiple_from(numbers, i, count, base, test, quotients);
}

static void divide_exact_scalar(const uint64_t* numbers, uint32_t count, uint64_t base,
                                const sx_exact_divisor* divisor, uint64_t* out) {
    if (divisor->inverse == 1) {
        /* A power of two (or 1): only the shift. */
        for (uint32_t i = 0; i < count; i++) {
            out[i] = (numbers[i] - base) >> divisor->shift;
        }
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        out[i] = sx_divide_exact(numbers[i] - base, divisor);
    }
}

static uint32_t width_counts_scalar(const uint64_t* numbers, uint32_t count, uint32_t max_width,
                                    uint32_t enough, uint32_t* counts) {
    memset(counts, 0, ((size_t)max_width + 1u) * sizeof(uint32_t));
    sx_add_width_counts(numbers, count, max_width, counts);
    return sx_width_counts_stop(counts, max_width, enough);
}

static uint32_t find_exceptions_scalar(const uint64_t* offsets, uint32_t count, uint64_t limit,
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

static void to_lane_order_scalar(const uint64_t* values, uint32_t count, uint64_t limit,
                                 uint64_t* out) {
    uint32_t rows = sx_bitpack_rows(count);
    memset(out, 0, (size_t)rows * SX_BITPACK_ROW_BYTES);
    /* Lane l takes the run of values starting at l * rows. Without a limit, the copy is a
     * plain one. */
    for (uint32_t lane = 0; lane < SX_BITPACK_LANES && lane * rows < count; lane++) {
        const uint64_t* run = values + (size_t)lane * rows;
        uint64_t* lane_out = out + lane;
        uint32_t run_length = count - lane * rows < rows ? count - lane * rows : rows;
        if (limit == UINT64_MAX) {
            for (uint32_t row = 0; row < run_length; row++) {
                lane_out[(size_t)row * SX_BITPACK_LANES] = run[row];
            }
        } else {
            for (uint32_t row = 0; row < run_length; row++) {
                lane_out[(size_t)row * SX_BITPACK_LANES] = run[row] > limit ? 0 : run[row];
            }
        }
    }
}

const sx_codec_kernels sx_kernels_scalar = {
    .level = SX_SIMD_SCALAR,
    .bitpack = bitpack_scalar,
    .bitunpack = bitunpack_scalar,
    .decode_for = decode_for_scalar,
    .decode_alp = decode_alp_scalar,
    .decode_deltas = decode_deltas_scalar,
    .rebuild_deltas = rebuild_deltas_scalar,
    .fill_arithmetic = fill_arithmetic_scalar,
    .encode_alp = encode_alp_scalar,
    .summarize_ints = summarize_ints_scalar,
    .first_non_multiple = first_non_multiple_scalar,
    .divide_exact = divide_exact_scalar,
    .width_counts = width_counts_scalar,
    .find_exceptions = find_exceptions_scalar,
    .to_lane_order = to_lane_order_scalar,
};
