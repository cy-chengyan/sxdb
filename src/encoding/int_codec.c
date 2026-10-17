/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "encoding/int_codec.h"

#include "common/bits.h"
#include "common/compiler.h"
#include "encoding/bitpack.h"
#include "encoding/kernels.h"

#include <stdbool.h>
#include <string.h>

#define CONSTANT_SIZE             8u
#define DELTA_CONST_SIZE          16u
#define RLE_HEADER_SIZE           8u
#define FOR_HEADER_SIZE           24u
#define DELTA_FOR_HEADER_SIZE     32u
#define DELTA_PATCHED_HEADER_SIZE 32u
#define HEADER_PADDING            7u

/* Facts about a chunk that decide which encoding is smallest. */
typedef struct int_stats {
    uint32_t count;
    int64_t min;
    int64_t max;
    uint32_t run_count;

    /* Frame of reference over the values: value = min + packed * for_multiplier. */
    uint64_t for_multiplier;
    uint32_t for_width;
    uint64_t for_offsets[SX_VECTOR_SIZE]; /* value - min, before division */

    /* Frame of reference over consecutive differences (only when count >= 2):
     * delta[i] = delta_min + delta_offsets[i] * delta_multiplier. */
    bool deltas_equal;
    int64_t delta_min;
    uint64_t delta_multiplier;
    uint32_t delta_width;
    uint64_t delta_offsets[SX_VECTOR_SIZE];

    /* DELTA_PATCHED: offsets wider than patch_width are stored as exceptions. */
    uint32_t patch_width;
    uint32_t patch_exceptions;
    size_t patch_size; /* 0 if patching does not beat DELTA_FOR */
} int_stats;

/* Difference between neighbors, wrapping modulo 2^64 and read back as signed. */
static int64_t wrapping_delta(int64_t current, int64_t previous) {
    return (int64_t)((uint64_t)current - (uint64_t)previous);
}

/* Unsigned distance from `base` up to `value`, where value >= base as signed numbers. */
static uint64_t offset_from(int64_t value, int64_t base) {
    return (uint64_t)value - (uint64_t)base;
}

/*
 * Division by a divisor that is known to divide every dividend exactly (the multiplier is
 * the greatest common divisor of the offsets). Shifting out the divisor's factors of two
 * and multiplying by the inverse of its odd part modulo 2^64 gives the exact quotient
 * with one shift and one multiplication instead of a slow division.
 */
typedef struct exact_divisor {
    uint32_t shift;
    uint64_t inverse;
} exact_divisor;

static exact_divisor make_exact_divisor(uint64_t divisor) {
    exact_divisor result = {.shift = 0, .inverse = 1};
    if (divisor == 0) {
        return result; /* every offset is zero */
    }
    result.shift = sx_trailing_zeros_u64(divisor);
    uint64_t odd = divisor >> result.shift;
    /* Newton's iteration doubles the correct low bits each step: 3, 6, 12, 24, 48, 96. */
    uint64_t inverse = odd;
    for (int step = 0; step < 5; step++) {
        inverse *= 2u - odd * inverse;
    }
    result.inverse = inverse;
    return result;
}

static uint64_t divide_exact(uint64_t dividend, exact_divisor divisor) {
    return (dividend >> divisor.shift) * divisor.inverse;
}

/*
 * Tests whether numbers are multiples of a divisor with one multiplication: x is a multiple
 * of d = odd * 2^shift exactly when its low `shift` bits are zero and
 * (x >> shift) * inverse(odd) (mod 2^64) is at most (2^64 - 1) / odd.
 */
typedef struct multiple_test {
    exact_divisor exact;
    uint64_t low_bits;     /* mask of the low `shift` bits */
    uint64_t max_quotient; /* (2^64 - 1) / odd */
} multiple_test;

static multiple_test make_multiple_test(uint64_t divisor) {
    multiple_test test;
    test.exact = make_exact_divisor(divisor);
    test.low_bits = sx_low_mask_u64(test.exact.shift);
    test.max_quotient = UINT64_MAX / (divisor >> test.exact.shift);
    return test;
}

/* Zero exactly when x is a multiple; computed without branches. */
static uint64_t multiple_miss(uint64_t x, const multiple_test* test) {
    uint64_t quotient = (x >> test->exact.shift) * test->exact.inverse;
    return (x & test->low_bits) | (uint64_t)(quotient > test->max_quotient);
}

/*
 * Index of the first number from `start` on that is not a multiple, or `count`. Blocks of
 * 16 numbers are tested with one branch, since nearly all numbers are multiples.
 */
static uint32_t first_non_multiple(const uint64_t* numbers, uint32_t start, uint32_t count,
                                   const multiple_test* test) {
    uint32_t i = start;
    while (i + 16u <= count) {
        uint64_t misses = 0;
        for (uint32_t j = 0; j < 16u; j++) {
            misses |= multiple_miss(numbers[i + j], test);
        }
        if (misses != 0) {
            break;
        }
        i += 16u;
    }
    while (i < count && multiple_miss(numbers[i], test) == 0) {
        i++;
    }
    return i;
}

/*
 * Greatest common divisor of `count` numbers, 0 if they are all 0. Most numbers are
 * multiples of the divisor found so far, which multiple_miss confirms cheaply; only the
 * others need a gcd computation. The divisor only shrinks, so that happens rarely, and
 * once it reaches 1 the scan stops.
 */
static uint64_t common_divisor(const uint64_t* numbers, uint32_t count) {
    uint32_t i = 0;
    while (i < count && numbers[i] == 0) {
        i++;
    }
    if (i == count) {
        return 0;
    }
    uint64_t divisor = numbers[i];
    while (divisor != 1) {
        multiple_test test = make_multiple_test(divisor);
        i = first_non_multiple(numbers, i + 1u, count, &test);
        if (i == count) {
            break;
        }
        divisor = sx_gcd_u64(divisor, numbers[i]);
    }
    return divisor;
}

/* Bit width of the largest offset after dividing by the multiplier. */
static uint32_t packed_width(uint64_t range, uint64_t multiplier) {
    if (multiplier == 0) {
        return 0;
    }
    return sx_bit_width_u64(range / multiplier);
}

static int64_t min_i64(int64_t a, int64_t b) {
    return b < a ? b : a;
}

static int64_t max_i64(int64_t a, int64_t b) {
    return b > a ? b : a;
}

static void compute_stats(const int64_t* values, uint32_t count, int_stats* stats) {
    /* Running values are kept in locals: `values` could alias `stats`, which would make
     * the compiler store every update. The minimum and maximum are conditional moves. */
    int64_t min = values[0];
    int64_t max = values[0];
    int64_t delta_min = count >= 2 ? wrapping_delta(values[1], values[0]) : 0;
    int64_t delta_max = delta_min;
    uint32_t value_changes = 0;
    for (uint32_t i = 1; i < count; i++) {
        int64_t value = values[i];
        int64_t delta = wrapping_delta(value, values[i - 1]);
        min = min_i64(min, value);
        max = max_i64(max, value);
        delta_min = min_i64(delta_min, delta);
        delta_max = max_i64(delta_max, delta);
        value_changes += (uint32_t)(value != values[i - 1]);
    }
    stats->count = count;
    stats->min = min;
    stats->max = max;
    stats->run_count = 1 + value_changes;
    stats->deltas_equal = count >= 2 && delta_min == delta_max;
    stats->delta_min = delta_min;
    stats->patch_width = 0;
    stats->patch_exceptions = 0;
    stats->patch_size = 0;

    /* The multipliers are the common divisors of the offsets from each frame's base. */
    for (uint32_t i = 0; i < count; i++) {
        stats->for_offsets[i] = offset_from(values[i], min);
    }
    stats->for_multiplier = common_divisor(stats->for_offsets, count);
    for (uint32_t i = 1; i < count; i++) {
        int64_t delta = wrapping_delta(values[i], values[i - 1]);
        stats->delta_offsets[i - 1] = offset_from(delta, delta_min);
    }
    stats->delta_multiplier = common_divisor(stats->delta_offsets, count - 1);

    stats->for_width = packed_width(offset_from(max, min), stats->for_multiplier);
    stats->delta_width = packed_width(offset_from(delta_max, delta_min), stats->delta_multiplier);

    exact_divisor divisor = make_exact_divisor(stats->delta_multiplier);
    for (uint32_t i = 0; i + 1 < count; i++) {
        stats->delta_offsets[i] = divide_exact(stats->delta_offsets[i], divisor);
    }
}

/* Payload size of DELTA_PATCHED with `exceptions` offsets stored apart. */
static size_t patched_size(uint32_t delta_count, uint32_t width, uint32_t exceptions) {
    return DELTA_PATCHED_HEADER_SIZE + sx_bitpack_size(delta_count, width) +
           sx_align_up((size_t)exceptions * sizeof(uint16_t), 8) +
           (size_t)exceptions * sizeof(uint64_t);
}

/*
 * Picks the packed width for DELTA_PATCHED. A few large gaps (a lunch break, a night, a
 * weekend) would force every delta to their width; storing them apart lets the rest be
 * packed narrowly. Every width below the full one is tried using a histogram of widths.
 */
static void choose_patch_width(int_stats* stats) {
    stats->patch_size = 0;
    if (stats->count < 2 || stats->delta_width == 0) {
        return;
    }
    uint32_t delta_count = stats->count - 1;
    /* Neighboring offsets usually have the same width; four separate histograms keep the
     * increments of one counter from waiting on each other. */
    uint32_t counts0[65] = {0};
    uint32_t counts1[65] = {0};
    uint32_t counts2[65] = {0};
    uint32_t counts3[65] = {0};
    const uint64_t* offsets = stats->delta_offsets;
    uint32_t i = 0;
    for (; i + 4u <= delta_count; i += 4u) {
        counts0[sx_bit_width_u64(offsets[i])]++;
        counts1[sx_bit_width_u64(offsets[i + 1u])]++;
        counts2[sx_bit_width_u64(offsets[i + 2u])]++;
        counts3[sx_bit_width_u64(offsets[i + 3u])]++;
    }
    for (; i < delta_count; i++) {
        counts0[sx_bit_width_u64(offsets[i])]++;
    }
    uint32_t width_counts[65];
    for (uint32_t width = 0; width <= 64; width++) {
        width_counts[width] = counts0[width] + counts1[width] + counts2[width] + counts3[width];
    }

    size_t unpatched = DELTA_FOR_HEADER_SIZE + sx_bitpack_size(delta_count, stats->delta_width);
    size_t best_size = unpatched;
    uint32_t wider = delta_count; /* offsets wider than `width` */
    for (uint32_t width = 0; width < stats->delta_width; width++) {
        wider -= width_counts[width];
        size_t size = patched_size(delta_count, width, wider);
        if (size < best_size) {
            best_size = size;
            stats->patch_width = width;
            stats->patch_exceptions = wider;
        }
    }
    if (best_size < unpatched) {
        stats->patch_size = best_size;
    }
}

/* Payload size of `encoding` for the chunk, or 0 if the encoding does not apply. */
static size_t encoded_size(sx_encoding encoding, const int_stats* stats) {
    uint32_t count = stats->count;
    switch (encoding) {
        case SX_ENCODING_RAW:
            return (size_t)count * sizeof(int64_t);
        case SX_ENCODING_CONSTANT:
            return stats->min == stats->max ? CONSTANT_SIZE : 0;
        case SX_ENCODING_DELTA_CONST:
            return count >= 2 && stats->deltas_equal ? DELTA_CONST_SIZE : 0;
        case SX_ENCODING_RLE:
            return RLE_HEADER_SIZE + (size_t)stats->run_count * sizeof(int64_t) +
                   sx_align_up((size_t)stats->run_count * sizeof(uint16_t), 8);
        case SX_ENCODING_FOR_BITPACK:
            return FOR_HEADER_SIZE + sx_bitpack_size(count, stats->for_width);
        case SX_ENCODING_DELTA_FOR:
            if (count < 2) {
                return 0;
            }
            return DELTA_FOR_HEADER_SIZE + sx_bitpack_size(count - 1, stats->delta_width);
        case SX_ENCODING_DELTA_PATCHED:
            return stats->patch_size;
        default:
            return 0;
    }
}

/* ---------------------------------------------------------------------------------------- */
/* Encoders                                                                                 */
/* ---------------------------------------------------------------------------------------- */

static void write_padding(sx_byte_writer* out, size_t count) {
    sx_writer_reserve_zeroed(out, count);
}

static void write_packed(sx_byte_writer* out, const uint64_t* offsets, uint32_t count,
                         uint32_t width) {
    uint8_t* packed = sx_writer_reserve(out, sx_bitpack_size(count, width));
    if (packed != NULL) {
        sx_bitpack_u64(offsets, count, width, packed);
    }
}

/* Packs delta offsets in the lane-contiguous order (see bitpack.h). */
static void write_packed_deltas(sx_byte_writer* out, const uint64_t* offsets, uint32_t count,
                                uint32_t width) {
    uint64_t lane_ordered[SX_VECTOR_SIZE];
    sx_bitpack_to_lane_order(offsets, count, lane_ordered);
    write_packed(out, lane_ordered, sx_bitpack_padded_count(count), width);
}

static void encode_for(const int_stats* stats, sx_byte_writer* out) {
    uint64_t offsets[SX_VECTOR_SIZE];
    exact_divisor divisor = make_exact_divisor(stats->for_multiplier);
    for (uint32_t i = 0; i < stats->count; i++) {
        offsets[i] = divide_exact(stats->for_offsets[i], divisor);
    }
    sx_writer_put_i64(out, stats->min);
    sx_writer_put_u64(out, stats->for_multiplier);
    sx_writer_put_u8(out, (uint8_t)stats->for_width);
    write_padding(out, HEADER_PADDING);
    write_packed(out, offsets, stats->count, stats->for_width);
}

static void encode_delta_for(const int64_t* values, const int_stats* stats, sx_byte_writer* out) {
    sx_writer_put_i64(out, values[0]);
    sx_writer_put_i64(out, stats->delta_min);
    sx_writer_put_u64(out, stats->delta_multiplier);
    sx_writer_put_u8(out, (uint8_t)stats->delta_width);
    write_padding(out, HEADER_PADDING);
    write_packed_deltas(out, stats->delta_offsets, stats->count - 1, stats->delta_width);
}

static void encode_delta_patched(const int64_t* values, const int_stats* stats,
                                 sx_byte_writer* out) {
    uint32_t delta_count = stats->count - 1;
    uint64_t packed[SX_VECTOR_SIZE];
    uint16_t exception_positions[SX_VECTOR_SIZE];
    uint64_t exception_offsets[SX_VECTOR_SIZE];
    uint32_t exceptions = 0;
    for (uint32_t i = 0; i < delta_count; i++) {
        uint64_t offset = stats->delta_offsets[i];
        if (sx_bit_width_u64(offset) > stats->patch_width) {
            exception_positions[exceptions] = (uint16_t)i;
            exception_offsets[exceptions] = offset;
            exceptions++;
            packed[i] = 0;
        } else {
            packed[i] = offset;
        }
    }

    sx_writer_put_i64(out, values[0]);
    sx_writer_put_i64(out, stats->delta_min);
    sx_writer_put_u64(out, stats->delta_multiplier);
    sx_writer_put_u8(out, (uint8_t)stats->patch_width);
    write_padding(out, 1);
    sx_writer_put_u16(out, (uint16_t)exceptions);
    write_padding(out, 4);
    write_packed_deltas(out, packed, delta_count, stats->patch_width);
    sx_writer_put_bytes(out, exception_positions, (size_t)exceptions * sizeof(uint16_t));
    sx_writer_pad_to(out, 8);
    sx_writer_put_bytes(out, exception_offsets, (size_t)exceptions * sizeof(uint64_t));
}

static void encode_rle(const int64_t* values, uint32_t count, sx_byte_writer* out) {
    int64_t run_values[SX_VECTOR_SIZE];
    uint16_t run_lengths[SX_VECTOR_SIZE];
    uint32_t runs = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (runs > 0 && values[i] == run_values[runs - 1]) {
            run_lengths[runs - 1]++;
        } else {
            run_values[runs] = values[i];
            run_lengths[runs] = 1;
            runs++;
        }
    }
    sx_writer_put_u16(out, (uint16_t)runs);
    write_padding(out, RLE_HEADER_SIZE - sizeof(uint16_t));
    sx_writer_put_bytes(out, run_values, (size_t)runs * sizeof(int64_t));
    sx_writer_put_bytes(out, run_lengths, (size_t)runs * sizeof(uint16_t));
    sx_writer_pad_to(out, 8);
}

static sx_err encode_with(sx_encoding encoding, const int64_t* values, const int_stats* stats,
                          sx_byte_writer* out) {
    if (encoded_size(encoding, stats) == 0) {
        return SX_ERR_UNSUPPORTED;
    }
    size_t start = out->size;
    switch (encoding) {
        case SX_ENCODING_RAW:
            sx_writer_put_bytes(out, values, (size_t)stats->count * sizeof(int64_t));
            break;
        case SX_ENCODING_CONSTANT:
            sx_writer_put_i64(out, values[0]);
            break;
        case SX_ENCODING_DELTA_CONST:
            sx_writer_put_i64(out, values[0]);
            sx_writer_put_i64(out, stats->delta_min);
            break;
        case SX_ENCODING_RLE:
            encode_rle(values, stats->count, out);
            break;
        case SX_ENCODING_FOR_BITPACK:
            encode_for(stats, out);
            break;
        case SX_ENCODING_DELTA_FOR:
            encode_delta_for(values, stats, out);
            break;
        case SX_ENCODING_DELTA_PATCHED:
            encode_delta_patched(values, stats, out);
            break;
        default:
            return SX_ERR_UNSUPPORTED;
    }
    if (out->overflow) {
        out->size = start;
        out->overflow = false;
        return SX_ERR_LIMIT;
    }
    return SX_OK;
}

size_t sx_int64_encode_bound(uint32_t count) {
    /* The choice is never larger than RAW, which is always applicable. */
    return (size_t)count * sizeof(int64_t);
}

sx_err sx_int64_encode(const int64_t* values, uint32_t count, sx_byte_writer* out,
                       sx_encoding* chosen) {
    if (count == 0 || count > SX_VECTOR_SIZE) {
        return SX_ERR_INVALID_ARG;
    }
    int_stats stats;
    compute_stats(values, count, &stats);
    choose_patch_width(&stats);

    /* Candidates in order of decoding speed; a later one must be strictly smaller. */
    static const sx_encoding candidates[] = {
        SX_ENCODING_CONSTANT,  SX_ENCODING_DELTA_CONST,   SX_ENCODING_FOR_BITPACK,
        SX_ENCODING_DELTA_FOR, SX_ENCODING_DELTA_PATCHED, SX_ENCODING_RLE,
        SX_ENCODING_RAW,
    };
    sx_encoding best = SX_ENCODING_RAW;
    size_t best_size = SIZE_MAX;
    for (size_t i = 0; i < SX_ARRAY_SIZE(candidates); i++) {
        size_t size = encoded_size(candidates[i], &stats);
        if (size != 0 && size < best_size) {
            best = candidates[i];
            best_size = size;
        }
    }

    sx_err err = encode_with(best, values, &stats, out);
    if (err == SX_OK) {
        *chosen = best;
    }
    return err;
}

sx_err sx_int64_encode_as(sx_encoding encoding, const int64_t* values, uint32_t count,
                          sx_byte_writer* out) {
    if (count == 0 || count > SX_VECTOR_SIZE) {
        return SX_ERR_INVALID_ARG;
    }
    int_stats stats;
    compute_stats(values, count, &stats);
    choose_patch_width(&stats);
    return encode_with(encoding, values, &stats, out);
}

/* ---------------------------------------------------------------------------------------- */
/* Decoders                                                                                 */
/* ---------------------------------------------------------------------------------------- */

/* Takes the block of `count` values packed at a header's width; NULL if malformed. */
static const uint8_t* take_packed(sx_byte_reader* in, uint32_t count, uint32_t width) {
    if (width > 64) {
        return NULL;
    }
    return sx_reader_take(in, sx_bitpack_size(count, width));
}

static bool decode_raw(sx_byte_reader* in, uint32_t count, int64_t* out) {
    const uint8_t* src = sx_reader_take(in, (size_t)count * sizeof(int64_t));
    if (src == NULL) {
        return false;
    }
    memcpy(out, src, (size_t)count * sizeof(int64_t));
    return true;
}

static bool decode_constant(sx_byte_reader* in, uint32_t count, int64_t* out) {
    int64_t value = sx_reader_get_i64(in);
    for (uint32_t i = 0; i < count; i++) {
        out[i] = value;
    }
    return true;
}

static bool decode_delta_const(sx_byte_reader* in, uint32_t count, int64_t* out) {
    uint64_t first = sx_reader_get_u64(in);
    uint64_t step = sx_reader_get_u64(in);
    for (uint32_t i = 0; i < count; i++) {
        out[i] = (int64_t)(first + (uint64_t)i * step);
    }
    return true;
}

static bool decode_rle(sx_byte_reader* in, uint32_t count, int64_t* out) {
    uint32_t runs = sx_reader_get_u16(in);
    sx_reader_take(in, RLE_HEADER_SIZE - sizeof(uint16_t));
    if (runs == 0 || runs > count) {
        return false;
    }
    const uint8_t* run_values = sx_reader_take(in, (size_t)runs * sizeof(int64_t));
    const uint8_t* run_lengths = sx_reader_take(in, (size_t)runs * sizeof(uint16_t));
    sx_reader_skip_to(in, 8);
    if (run_values == NULL || run_lengths == NULL) {
        return false;
    }

    uint32_t written = 0;
    for (uint32_t run = 0; run < runs; run++) {
        int64_t value = (int64_t)sx_load_u64(run_values + run * sizeof(int64_t));
        uint32_t length = sx_load_u16(run_lengths + run * sizeof(uint16_t));
        if (length == 0 || length > count - written) {
            return false;
        }
        for (uint32_t i = 0; i < length; i++) {
            out[written + i] = value;
        }
        written += length;
    }
    return written == count;
}

static bool decode_for(sx_byte_reader* in, uint32_t count, int64_t* out) {
    uint64_t base = sx_reader_get_u64(in);
    uint64_t multiplier = sx_reader_get_u64(in);
    uint32_t width = sx_reader_get_u8(in);
    sx_reader_take(in, HEADER_PADDING);

    const uint8_t* packed = take_packed(in, count, width);
    if (packed == NULL) {
        return false;
    }
    sx_codec_kernels_active()->decode_for(packed, count, width, base, multiplier, (uint64_t*)out);
    return true;
}

/*
 * The deltas of DELTA_FOR and DELTA_PATCHED are delta_base + offset * multiplier, with the
 * `count - 1` offsets packed in the lane-contiguous order. They are decoded like
 * FOR_BITPACK into `deltas`, patched if needed, and then summed up by rebuild_deltas.
 */
static bool decode_delta_for(sx_byte_reader* in, uint32_t count, int64_t* out) {
    uint64_t first = sx_reader_get_u64(in);
    uint64_t delta_base = sx_reader_get_u64(in);
    uint64_t multiplier = sx_reader_get_u64(in);
    uint32_t width = sx_reader_get_u8(in);
    sx_reader_take(in, HEADER_PADDING);
    if (count < 2) {
        return false;
    }

    uint32_t delta_count = count - 1;
    const uint8_t* packed = take_packed(in, delta_count, width);
    if (packed == NULL) {
        return false;
    }
    const sx_codec_kernels* kernels = sx_codec_kernels_active();
    uint64_t deltas[SX_VECTOR_SIZE];
    kernels->decode_for(packed, delta_count, width, delta_base, multiplier, deltas);
    kernels->rebuild_deltas(deltas, delta_count, first, (uint64_t*)out);
    return true;
}

static bool decode_delta_patched(sx_byte_reader* in, uint32_t count, int64_t* out) {
    uint64_t first = sx_reader_get_u64(in);
    uint64_t delta_base = sx_reader_get_u64(in);
    uint64_t multiplier = sx_reader_get_u64(in);
    uint32_t width = sx_reader_get_u8(in);
    sx_reader_take(in, 1);
    uint32_t exceptions = sx_reader_get_u16(in);
    sx_reader_take(in, 4);
    if (count < 2 || exceptions > count - 1) {
        return false;
    }

    uint32_t delta_count = count - 1;
    const uint8_t* packed = take_packed(in, delta_count, width);
    const uint8_t* positions = sx_reader_take(in, (size_t)exceptions * sizeof(uint16_t));
    sx_reader_skip_to(in, 8);
    const uint8_t* patches = sx_reader_take(in, (size_t)exceptions * sizeof(uint64_t));
    if (packed == NULL || positions == NULL || patches == NULL) {
        return false;
    }
    const sx_codec_kernels* kernels = sx_codec_kernels_active();
    uint64_t deltas[SX_VECTOR_SIZE];
    kernels->decode_for(packed, delta_count, width, delta_base, multiplier, deltas);

    /* An exception replaces the offset of its slot (whose packed value is 0). */
    uint32_t rows = sx_bitpack_rows(delta_count);
    for (uint32_t i = 0; i < exceptions; i++) {
        uint32_t position = sx_load_u16(positions + i * sizeof(uint16_t));
        if (position >= delta_count) {
            return false;
        }
        uint64_t offset = sx_load_u64(patches + i * sizeof(uint64_t));
        deltas[sx_bitpack_lane_position(position, rows)] = delta_base + offset * multiplier;
    }
    kernels->rebuild_deltas(deltas, delta_count, first, (uint64_t*)out);
    return true;
}

sx_err sx_int64_decode(sx_encoding encoding, const uint8_t* data, size_t size, uint32_t count,
                       int64_t* out) {
    if (count == 0 || count > SX_VECTOR_SIZE) {
        return SX_ERR_INVALID_ARG;
    }
    sx_byte_reader in;
    sx_reader_init(&in, data, size);

    bool ok = false;
    switch (encoding) {
        case SX_ENCODING_RAW:
            ok = decode_raw(&in, count, out);
            break;
        case SX_ENCODING_CONSTANT:
            ok = decode_constant(&in, count, out);
            break;
        case SX_ENCODING_DELTA_CONST:
            ok = decode_delta_const(&in, count, out);
            break;
        case SX_ENCODING_RLE:
            ok = decode_rle(&in, count, out);
            break;
        case SX_ENCODING_FOR_BITPACK:
            ok = decode_for(&in, count, out);
            break;
        case SX_ENCODING_DELTA_FOR:
            ok = decode_delta_for(&in, count, out);
            break;
        case SX_ENCODING_DELTA_PATCHED:
            ok = decode_delta_patched(&in, count, out);
            break;
        default:
            return SX_ERR_CORRUPT;
    }
    if (!ok || in.failed || sx_reader_remaining(&in) != 0) {
        return SX_ERR_CORRUPT;
    }
    return SX_OK;
}
