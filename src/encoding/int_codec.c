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

/*
 * Facts about a chunk that decide which encoding is smallest. The summary comes first;
 * the frames of reference are computed only when the summary does not settle the choice
 * (see sx_int64_encode).
 */
typedef struct int_stats {
    const sx_codec_kernels* kernels; /* the kernel set, looked up once per chunk */
    uint32_t count;
    int64_t min;
    int64_t max;
    uint32_t run_count;
    bool deltas_equal; /* count >= 2 and all deltas are equal */
    int64_t delta_min;
    int64_t delta_max;

    /* Frame of reference over the values: value = min + packed * for_multiplier. */
    uint64_t for_multiplier;
    uint32_t for_width;

    /* Frame of reference over consecutive differences (only when count >= 2):
     * deltas[i] = delta_min + delta_offsets[i] * delta_multiplier. */
    uint64_t delta_multiplier;
    uint32_t delta_width;
    uint64_t deltas[SX_VECTOR_SIZE];
    uint64_t delta_offsets[SX_VECTOR_SIZE];

    /* DELTA_PATCHED: offsets wider than patch_width are stored as exceptions. */
    uint32_t patch_width;
    uint32_t patch_exceptions;
    size_t patch_size; /* 0 if patching does not beat DELTA_FOR */
} int_stats;

/* Unsigned distance from `base` up to `value`, where value >= base as signed numbers. */
static uint64_t offset_from(int64_t value, int64_t base) {
    return (uint64_t)value - (uint64_t)base;
}

/*
 * Greatest common divisor of numbers[i] - base for i < count, 0 if they are all 0; the
 * quotients by it go to `quotients`. Most numbers are multiples of the divisor found so
 * far, which first_non_multiple confirms cheaply; only the others need a gcd computation.
 * The divisor only shrinks, so that happens rarely, and once it reaches 1 the scan stops.
 * It starts at 0, the divisor of no numbers, whose only multiple is 0.
 *
 * The test computes the quotients by the divisor of the time. Those from the last change
 * of the divisor to the end are final; the numbers before it, and those after the scan
 * stopped at 1, are divided afterwards.
 */
static uint64_t common_divisor(const sx_codec_kernels* kernels, const uint64_t* numbers,
                               uint32_t count, uint64_t base, uint64_t* quotients) {
    uint64_t divisor = 0;
    sx_exact_divisor exact = {.shift = 0, .inverse = 1}; /* for 0 and 1: no division */
    uint32_t settled = 0; /* quotients[settled, tested) are by the current divisor */
    uint32_t tested = 0;
    while (divisor != 1) {
        sx_multiple_test test = sx_multiple_test_make(divisor);
        uint32_t miss =
            kernels->first_non_multiple(numbers, settled, count, base, &test, quotients);
        if (miss == count) {
            tested = count;
            exact = test.exact;
            break;
        }
        divisor = sx_gcd_u64(divisor, numbers[miss] - base);
        settled = miss + 1u;
        tested = settled;
    }
    kernels->divide_exact(numbers, settled, base, &exact, quotients);
    kernels->divide_exact(numbers + tested, count - tested, base, &exact, quotients + tested);
    return divisor;
}

/*
 * The FOR multiplier: the greatest common divisor of the offsets values[i] - min. Each
 * offset is a sum of deltas and each delta a difference of two offsets, so the offsets
 * and the deltas have the same common divisors. Every delta is delta_min plus a multiple
 * of the delta multiplier, so their gcd is gcd(|delta_min|, delta_multiplier), and no
 * scan is needed. That reasoning is about exact differences: it holds when no delta
 * wraps around, which a value range below 2^63 guarantees. Otherwise the offsets are
 * scanned.
 */
static uint64_t for_multiplier(const sx_codec_kernels* kernels, const int64_t* values,
                               const int_stats* stats) {
    if (offset_from(stats->max, stats->min) <= (uint64_t)INT64_MAX) {
        uint64_t delta_min = (uint64_t)stats->delta_min;
        uint64_t magnitude = stats->delta_min < 0 ? 0u - delta_min : delta_min;
        return sx_gcd_u64(magnitude, stats->delta_multiplier);
    }
    uint64_t quotients[SX_VECTOR_SIZE]; /* not needed */
    return common_divisor(kernels, (const uint64_t*)values, stats->count, (uint64_t)stats->min,
                          quotients);
}

/* Bit width of the largest offset after dividing by the multiplier. */
static uint32_t packed_width(uint64_t range, uint64_t multiplier) {
    if (multiplier <= 1) {
        return multiplier == 0 ? 0 : sx_bit_width_u64(range); /* without a slow division */
    }
    return sx_bit_width_u64(range / multiplier);
}

/* The summary: range, runs, and deltas. */
static void summarize(const int64_t* values, uint32_t count, int_stats* stats) {
    stats->kernels = sx_codec_kernels_active();
    sx_int_summary summary;
    stats->kernels->summarize_ints(values, count, stats->deltas, &summary);
    stats->count = count;
    stats->min = summary.min;
    stats->max = summary.max;
    stats->run_count = 1 + summary.changes;
    stats->deltas_equal = count >= 2 && summary.delta_min == summary.delta_max;
    stats->delta_min = summary.delta_min;
    stats->delta_max = summary.delta_max;
}

/* Size of the exceptions of DELTA_PATCHED: positions, padding, and offsets. */
static size_t exceptions_size(uint32_t exceptions) {
    return sx_align_up((size_t)exceptions * sizeof(uint16_t), 8) +
           (size_t)exceptions * sizeof(uint64_t);
}

/* Payload size of DELTA_PATCHED with `exceptions` offsets stored apart. */
static size_t patched_size(uint32_t delta_count, uint32_t width, uint32_t exceptions) {
    return DELTA_PATCHED_HEADER_SIZE + sx_bitpack_size(delta_count, width) +
           exceptions_size(exceptions);
}

/* The fewest exceptions whose storage alone takes at least `size` bytes. */
static uint32_t exceptions_filling(size_t size) {
    /* Each exception takes 10 bytes, plus at most 6 of padding in all. */
    uint32_t exceptions = (uint32_t)(size / 10u);
    while (exceptions_size(exceptions) < size) {
        exceptions++;
    }
    return exceptions;
}

/*
 * Picks the packed width for DELTA_PATCHED. A few large gaps (a lunch break, a night, a
 * weekend) would force every delta to their width; storing them apart lets the rest be
 * packed narrowly. Every width below the full one is tried using a histogram of widths.
 *
 * The histogram is counted from the widest offsets down, and only as far as it matters:
 * once the offsets at least w bits wide are so many that storing them as exceptions takes
 * as much as packing everything at the full width, no width below w can beat DELTA_FOR.
 */
static void choose_patch_width(int_stats* stats) {
    stats->patch_width = 0;
    stats->patch_exceptions = 0;
    stats->patch_size = 0;
    if (stats->count < 2 || stats->delta_width == 0) {
        return;
    }
    uint32_t delta_count = stats->count - 1;
    size_t unpatched = DELTA_FOR_HEADER_SIZE + sx_bitpack_size(delta_count, stats->delta_width);
    uint32_t enough = exceptions_filling(unpatched - DELTA_PATCHED_HEADER_SIZE);
    uint32_t width_counts[65];
    uint32_t lowest = stats->kernels->width_counts(stats->delta_offsets, delta_count,
                                                   stats->delta_width, enough, width_counts);

    size_t best_size = unpatched;
    /* Offsets at least `lowest` bits wide; in the loop, offsets wider than `width`. */
    uint32_t wider = 0;
    for (uint32_t width = lowest; width <= stats->delta_width; width++) {
        wider += width_counts[width];
    }
    for (uint32_t width = lowest; width < stats->delta_width; width++) {
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

/* The frames of reference, after summarize. */
static void compute_frames(const int64_t* values, int_stats* stats) {
    const sx_codec_kernels* kernels = stats->kernels;
    uint32_t delta_count = stats->count - 1;
    uint64_t delta_base = (uint64_t)stats->delta_min;
    /* The multipliers are the common divisors of the offsets from each frame's base. */
    stats->delta_multiplier =
        common_divisor(kernels, stats->deltas, delta_count, delta_base, stats->delta_offsets);
    stats->for_multiplier = for_multiplier(kernels, values, stats);
    stats->for_width = packed_width(offset_from(stats->max, stats->min), stats->for_multiplier);
    stats->delta_width =
        packed_width(offset_from(stats->delta_max, stats->delta_min), stats->delta_multiplier);
    choose_patch_width(stats);
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
            return stats->deltas_equal ? DELTA_CONST_SIZE : 0;
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

/* Packs (values[i] - base) for i < count at `width` bits. */
static void write_packed(const sx_codec_kernels* kernels, sx_byte_writer* out,
                         const uint64_t* values, uint32_t count, uint32_t width, uint64_t base) {
    uint8_t* packed = sx_writer_reserve(out, sx_bitpack_size(count, width));
    if (packed != NULL) {
        kernels->bitpack(values, count, width, base, packed);
    }
}

/*
 * Packs delta offsets in the lane-contiguous order (see bitpack.h), those above `limit` as
 * 0.
 */
static void write_packed_deltas(const sx_codec_kernels* kernels, sx_byte_writer* out,
                                const uint64_t* offsets, uint32_t count, uint32_t width,
                                uint64_t limit) {
    if (width == 0) {
        return; /* nothing is stored, as with regular timestamps and DELTA_PATCHED */
    }
    uint64_t lane_ordered[SX_VECTOR_SIZE];
    kernels->to_lane_order(offsets, count, limit, lane_ordered);
    write_packed(kernels, out, lane_ordered, sx_bitpack_padded_count(count), width, 0);
}

static void encode_for(const int64_t* values, const int_stats* stats, sx_byte_writer* out) {
    sx_writer_put_i64(out, stats->min);
    sx_writer_put_u64(out, stats->for_multiplier);
    sx_writer_put_u8(out, (uint8_t)stats->for_width);
    write_padding(out, HEADER_PADDING);
    if (stats->for_multiplier <= 1) {
        /* The offsets are the values minus min, which the packing subtracts itself. */
        write_packed(stats->kernels, out, (const uint64_t*)values, stats->count, stats->for_width,
                     (uint64_t)stats->min);
        return;
    }
    uint64_t offsets[SX_VECTOR_SIZE];
    sx_exact_divisor divisor = sx_exact_divisor_make(stats->for_multiplier);
    stats->kernels->divide_exact((const uint64_t*)values, stats->count, (uint64_t)stats->min,
                                 &divisor, offsets);
    write_packed(stats->kernels, out, offsets, stats->count, stats->for_width, 0);
}

static void encode_delta_for(const int64_t* values, const int_stats* stats, sx_byte_writer* out) {
    sx_writer_put_i64(out, values[0]);
    sx_writer_put_i64(out, stats->delta_min);
    sx_writer_put_u64(out, stats->delta_multiplier);
    sx_writer_put_u8(out, (uint8_t)stats->delta_width);
    write_padding(out, HEADER_PADDING);
    write_packed_deltas(stats->kernels, out, stats->delta_offsets, stats->count - 1,
                        stats->delta_width, UINT64_MAX);
}

static void encode_delta_patched(const int64_t* values, const int_stats* stats,
                                 sx_byte_writer* out) {
    uint32_t delta_count = stats->count - 1;
    const uint64_t* offsets = stats->delta_offsets;
    /* An offset is wider than patch_width bits exactly when it exceeds this limit. */
    uint64_t limit = sx_low_mask_u64(stats->patch_width);
    uint64_t exception_bits[SX_VECTOR_SIZE / 64u];
    uint32_t exceptions =
        stats->kernels->find_exceptions(offsets, delta_count, limit, exception_bits);
    /* The exceptions are rare: they are listed from their bits, in order. */
    uint16_t exception_positions[SX_VECTOR_SIZE];
    uint64_t exception_offsets[SX_VECTOR_SIZE];
    uint32_t listed = 0;
    for (uint32_t word = 0; word * 64u < delta_count; word++) {
        for (uint64_t bits = exception_bits[word]; bits != 0; bits &= bits - 1u) {
            uint32_t position = word * 64u + sx_trailing_zeros_u64(bits);
            exception_positions[listed] = (uint16_t)position;
            exception_offsets[listed] = offsets[position];
            listed++;
        }
    }

    sx_writer_put_i64(out, values[0]);
    sx_writer_put_i64(out, stats->delta_min);
    sx_writer_put_u64(out, stats->delta_multiplier);
    sx_writer_put_u8(out, (uint8_t)stats->patch_width);
    write_padding(out, 1);
    sx_writer_put_u16(out, (uint16_t)exceptions);
    write_padding(out, 4);
    write_packed_deltas(stats->kernels, out, offsets, delta_count, stats->patch_width, limit);
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
            encode_for(values, stats, out);
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

/*
 * The smallest applicable encoding; candidates come in order of decoding speed, and a
 * later one must be strictly smaller.
 */
static sx_encoding choose_encoding(const int_stats* stats) {
    static const sx_encoding candidates[] = {
        SX_ENCODING_CONSTANT,  SX_ENCODING_DELTA_CONST,   SX_ENCODING_FOR_BITPACK,
        SX_ENCODING_DELTA_FOR, SX_ENCODING_DELTA_PATCHED, SX_ENCODING_RLE,
        SX_ENCODING_RAW,
    };
    sx_encoding best = SX_ENCODING_RAW;
    size_t best_size = SIZE_MAX;
    for (size_t i = 0; i < SX_ARRAY_SIZE(candidates); i++) {
        size_t size = encoded_size(candidates[i], stats);
        if (size != 0 && size < best_size) {
            best = candidates[i];
            best_size = size;
        }
    }
    return best;
}

sx_err sx_int64_encode(const int64_t* values, uint32_t count, sx_byte_writer* out,
                       sx_encoding* chosen) {
    if (count == 0 || count > SX_VECTOR_SIZE) {
        return SX_ERR_INVALID_ARG;
    }
    int_stats stats;
    summarize(values, count, &stats);
    /*
     * CONSTANT (8 bytes) and DELTA_CONST (16 bytes) come first, and no later candidate is
     * smaller: FOR_BITPACK and RLE take at least 24 bytes, the delta encodings 32, and RAW
     * at least 16, with DELTA_CONST needing two values. So when either applies, the frames
     * of reference are not needed.
     */
    sx_encoding best;
    if (encoded_size(SX_ENCODING_CONSTANT, &stats) != 0) {
        best = SX_ENCODING_CONSTANT;
    } else if (encoded_size(SX_ENCODING_DELTA_CONST, &stats) != 0) {
        best = SX_ENCODING_DELTA_CONST;
    } else {
        compute_frames(values, &stats);
        best = choose_encoding(&stats);
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
    summarize(values, count, &stats);
    compute_frames(values, &stats);
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
