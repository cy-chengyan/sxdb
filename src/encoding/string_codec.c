/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "encoding/string_codec.h"

#include "common/bits.h"
#include "common/compiler.h"
#include "common/hash.h"
#include "encoding/bitpack.h"

#include <stdbool.h>
#include <string.h>

#define CONSTANT_HEADER_SIZE 8u
#define STRING_HEADER_SIZE   16u
#define DICT_HEADER_SIZE     16u
#define MAX_LENGTH_WIDTH     32u

/* Open-addressing table size for finding distinct strings: twice the maximum count. */
#define DICT_TABLE_SIZE (2u * SX_VECTOR_SIZE)

static uint32_t string_length(const uint32_t* offsets, uint32_t index) {
    return offsets[index + 1] - offsets[index];
}

/* Lengths are stored as base + packed offset. */
typedef struct length_frame {
    uint32_t base;
    uint32_t width;
} length_frame;

static length_frame frame_for_lengths(const uint32_t* lengths, uint32_t count) {
    uint32_t min = lengths[0];
    uint32_t max = lengths[0];
    for (uint32_t i = 1; i < count; i++) {
        if (lengths[i] < min) {
            min = lengths[i];
        }
        if (lengths[i] > max) {
            max = lengths[i];
        }
    }
    length_frame frame = {.base = min, .width = sx_bit_width_u64(max - min)};
    return frame;
}

/* Distinct strings of a chunk, in order of first appearance, and each value's code. */
typedef struct string_dictionary {
    uint32_t count;
    uint32_t first_index[SX_VECTOR_SIZE]; /* value index where each distinct string appears */
    uint32_t lengths[SX_VECTOR_SIZE];     /* length of each distinct string */
    uint64_t codes[SX_VECTOR_SIZE];       /* dictionary code of each value */
    uint64_t total_bytes;                 /* bytes of all distinct strings together */
} string_dictionary;

static bool strings_equal(const uint32_t* offsets, const uint8_t* data, uint32_t a, uint32_t b) {
    uint32_t length = string_length(offsets, a);
    if (length != string_length(offsets, b)) {
        return false;
    }
    return memcmp(data + offsets[a], data + offsets[b], length) == 0;
}

static void build_dictionary(const uint32_t* offsets, const uint8_t* data, uint32_t count,
                             string_dictionary* dict) {
    int32_t table[DICT_TABLE_SIZE];
    for (uint32_t i = 0; i < DICT_TABLE_SIZE; i++) {
        table[i] = -1;
    }
    dict->count = 0;
    dict->total_bytes = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t length = string_length(offsets, i);
        uint64_t hash = sx_hash_bytes(data + offsets[i], length, 0);
        uint32_t slot = (uint32_t)hash & (DICT_TABLE_SIZE - 1u);
        for (;;) {
            int32_t entry = table[slot];
            if (entry < 0) {
                uint32_t code = dict->count;
                table[slot] = (int32_t)code;
                dict->first_index[code] = i;
                dict->lengths[code] = length;
                dict->total_bytes += length;
                dict->codes[i] = code;
                dict->count++;
                break;
            }
            if (strings_equal(offsets, data, dict->first_index[entry], i)) {
                dict->codes[i] = (uint64_t)entry;
                break;
            }
            slot = (slot + 1u) & (DICT_TABLE_SIZE - 1u);
        }
    }
}

/* ---------------------------------------------------------------------------------------- */
/* Sizes and encoders                                                                       */
/* ---------------------------------------------------------------------------------------- */

typedef struct string_stats {
    uint32_t count;
    uint64_t total_bytes;
    uint32_t lengths[SX_VECTOR_SIZE];
    length_frame frame;
    string_dictionary dict;
    length_frame dict_frame;
    uint32_t code_width;
} string_stats;

static void compute_stats(const uint32_t* offsets, const uint8_t* data, uint32_t count,
                          string_stats* stats) {
    stats->count = count;
    stats->total_bytes = (uint64_t)offsets[count] - offsets[0];
    for (uint32_t i = 0; i < count; i++) {
        stats->lengths[i] = string_length(offsets, i);
    }
    stats->frame = frame_for_lengths(stats->lengths, count);
    build_dictionary(offsets, data, count, &stats->dict);
    stats->dict_frame = frame_for_lengths(stats->dict.lengths, stats->dict.count);
    stats->code_width = sx_bit_width_u64(stats->dict.count - 1u);
}

/* Payload size of `encoding`, or 0 if it does not apply. */
static size_t encoded_size(sx_encoding encoding, const string_stats* stats) {
    switch (encoding) {
        case SX_ENCODING_CONSTANT:
            if (stats->dict.count != 1) {
                return 0;
            }
            return CONSTANT_HEADER_SIZE + sx_align_up(stats->lengths[0], 8);
        case SX_ENCODING_STRING:
            if (stats->total_bytes > UINT32_MAX) {
                return 0;
            }
            return STRING_HEADER_SIZE + sx_bitpack_size(stats->count, stats->frame.width) +
                   sx_align_up((size_t)stats->total_bytes, 8);
        case SX_ENCODING_DICT:
            if (stats->dict.total_bytes > UINT32_MAX) {
                return 0;
            }
            return DICT_HEADER_SIZE + sx_bitpack_size(stats->dict.count, stats->dict_frame.width) +
                   sx_align_up((size_t)stats->dict.total_bytes, 8) +
                   sx_bitpack_size(stats->count, stats->code_width);
        default:
            return 0;
    }
}

size_t sx_string_encode_bound(const uint32_t* offsets, uint32_t count) {
    size_t total = (size_t)offsets[count] - offsets[0];
    return STRING_HEADER_SIZE + sx_bitpack_size(count, MAX_LENGTH_WIDTH) + sx_align_up(total, 8);
}

static void write_packed_lengths(sx_byte_writer* out, const uint32_t* lengths, uint32_t count,
                                 length_frame frame) {
    uint64_t offsets[SX_VECTOR_SIZE];
    for (uint32_t i = 0; i < count; i++) {
        offsets[i] = lengths[i] - frame.base;
    }
    uint8_t* packed = sx_writer_reserve(out, sx_bitpack_size(count, frame.width));
    if (packed != NULL) {
        sx_bitpack_u64(offsets, count, frame.width, packed);
    }
}

static void encode_constant(const uint32_t* offsets, const uint8_t* data, sx_byte_writer* out) {
    uint32_t length = string_length(offsets, 0);
    sx_writer_put_u32(out, length);
    sx_writer_reserve_zeroed(out, 4);
    sx_writer_put_bytes(out, data + offsets[0], length);
    sx_writer_pad_to(out, 8);
}

static void encode_string(const uint32_t* offsets, const uint8_t* data, const string_stats* stats,
                          sx_byte_writer* out) {
    sx_writer_put_u32(out, (uint32_t)stats->total_bytes);
    sx_writer_put_u32(out, stats->frame.base);
    sx_writer_put_u8(out, (uint8_t)stats->frame.width);
    sx_writer_reserve_zeroed(out, 7);
    write_packed_lengths(out, stats->lengths, stats->count, stats->frame);
    sx_writer_put_bytes(out, data + offsets[0], (size_t)stats->total_bytes);
    sx_writer_pad_to(out, 8);
}

static void encode_dict(const uint32_t* offsets, const uint8_t* data, const string_stats* stats,
                        sx_byte_writer* out) {
    const string_dictionary* dict = &stats->dict;
    sx_writer_put_u16(out, (uint16_t)dict->count);
    sx_writer_put_u8(out, (uint8_t)stats->code_width);
    sx_writer_put_u8(out, 0);
    sx_writer_put_u32(out, (uint32_t)dict->total_bytes);
    sx_writer_put_u32(out, stats->dict_frame.base);
    sx_writer_put_u8(out, (uint8_t)stats->dict_frame.width);
    sx_writer_reserve_zeroed(out, 3);
    write_packed_lengths(out, dict->lengths, dict->count, stats->dict_frame);
    for (uint32_t code = 0; code < dict->count; code++) {
        uint32_t index = dict->first_index[code];
        sx_writer_put_bytes(out, data + offsets[index], dict->lengths[code]);
    }
    sx_writer_pad_to(out, 8);
    uint8_t* packed = sx_writer_reserve(out, sx_bitpack_size(stats->count, stats->code_width));
    if (packed != NULL) {
        sx_bitpack_u64(dict->codes, stats->count, stats->code_width, packed);
    }
}

static sx_err encode_with(sx_encoding encoding, const uint32_t* offsets, const uint8_t* data,
                          const string_stats* stats, sx_byte_writer* out) {
    if (encoded_size(encoding, stats) == 0) {
        return SX_ERR_UNSUPPORTED;
    }
    size_t start = out->size;
    switch (encoding) {
        case SX_ENCODING_CONSTANT:
            encode_constant(offsets, data, out);
            break;
        case SX_ENCODING_STRING:
            encode_string(offsets, data, stats, out);
            break;
        case SX_ENCODING_DICT:
            encode_dict(offsets, data, stats, out);
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

sx_err sx_string_encode(const uint32_t* offsets, const uint8_t* data, uint32_t count,
                        sx_byte_writer* out, sx_encoding* chosen) {
    if (count == 0 || count > SX_VECTOR_SIZE) {
        return SX_ERR_INVALID_ARG;
    }
    string_stats stats;
    compute_stats(offsets, data, count, &stats);

    static const sx_encoding candidates[] = {
        SX_ENCODING_CONSTANT,
        SX_ENCODING_DICT,
        SX_ENCODING_STRING,
    };
    sx_encoding best = SX_ENCODING_STRING;
    size_t best_size = SIZE_MAX;
    for (size_t i = 0; i < SX_ARRAY_SIZE(candidates); i++) {
        size_t size = encoded_size(candidates[i], &stats);
        if (size != 0 && size < best_size) {
            best = candidates[i];
            best_size = size;
        }
    }
    if (best_size == SIZE_MAX) {
        return SX_ERR_UNSUPPORTED;
    }
    sx_err err = encode_with(best, offsets, data, &stats, out);
    if (err == SX_OK) {
        *chosen = best;
    }
    return err;
}

sx_err sx_string_encode_as(sx_encoding encoding, const uint32_t* offsets, const uint8_t* data,
                           uint32_t count, sx_byte_writer* out) {
    if (count == 0 || count > SX_VECTOR_SIZE) {
        return SX_ERR_INVALID_ARG;
    }
    string_stats stats;
    compute_stats(offsets, data, count, &stats);
    return encode_with(encoding, offsets, data, &stats, out);
}

/* ---------------------------------------------------------------------------------------- */
/* Decoders                                                                                 */
/* ---------------------------------------------------------------------------------------- */

/*
 * A payload parsed and validated, ready to be copied out. For every encoding the result
 * is described as a list of source strings: value i is source[i] with length[i].
 */
typedef struct parsed_strings {
    const uint8_t* sources[SX_VECTOR_SIZE];
    uint32_t lengths[SX_VECTOR_SIZE];
    uint64_t total_bytes;
} parsed_strings;

/* Reads `count` lengths stored as base + packed offset. */
static bool read_lengths(sx_byte_reader* in, uint32_t count, uint32_t base, uint32_t width,
                         uint64_t* lengths) {
    if (width > MAX_LENGTH_WIDTH) {
        return false;
    }
    const uint8_t* packed = sx_reader_take(in, sx_bitpack_size(count, width));
    if (packed == NULL) {
        return false;
    }
    sx_bitunpack_u64(packed, count, width, lengths);
    for (uint32_t i = 0; i < count; i++) {
        lengths[i] += base;
        if (lengths[i] > UINT32_MAX) {
            return false;
        }
    }
    return true;
}

/* Splits `bytes` (of `total` bytes) into strings of the given lengths. */
static bool split_bytes(const uint8_t* bytes, uint64_t total, const uint64_t* lengths,
                        uint32_t count, const uint8_t** sources) {
    uint64_t position = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (lengths[i] > total - position) {
            return false;
        }
        sources[i] = bytes + position;
        position += lengths[i];
    }
    return position == total;
}

static bool parse_constant(sx_byte_reader* in, uint32_t count, parsed_strings* parsed) {
    uint32_t length = sx_reader_get_u32(in);
    sx_reader_take(in, 4);
    const uint8_t* bytes = sx_reader_take(in, length);
    sx_reader_skip_to(in, 8);
    if (bytes == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        parsed->sources[i] = bytes;
        parsed->lengths[i] = length;
    }
    parsed->total_bytes = (uint64_t)length * count;
    return true;
}

static bool parse_string(sx_byte_reader* in, uint32_t count, parsed_strings* parsed) {
    uint32_t total = sx_reader_get_u32(in);
    uint32_t base = sx_reader_get_u32(in);
    uint32_t width = sx_reader_get_u8(in);
    sx_reader_take(in, 7);

    uint64_t lengths[SX_VECTOR_SIZE];
    if (!read_lengths(in, count, base, width, lengths)) {
        return false;
    }
    const uint8_t* bytes = sx_reader_take(in, total);
    sx_reader_skip_to(in, 8);
    if (bytes == NULL || !split_bytes(bytes, total, lengths, count, parsed->sources)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        parsed->lengths[i] = (uint32_t)lengths[i];
    }
    parsed->total_bytes = total;
    return true;
}

static bool parse_dict(sx_byte_reader* in, uint32_t count, parsed_strings* parsed) {
    uint32_t dict_count = sx_reader_get_u16(in);
    uint32_t code_width = sx_reader_get_u8(in);
    sx_reader_take(in, 1);
    uint32_t dict_total = sx_reader_get_u32(in);
    uint32_t dict_base = sx_reader_get_u32(in);
    uint32_t dict_width = sx_reader_get_u8(in);
    sx_reader_take(in, 3);
    if (dict_count == 0 || dict_count > count || code_width != sx_bit_width_u64(dict_count - 1u)) {
        return false;
    }

    uint64_t dict_lengths[SX_VECTOR_SIZE];
    const uint8_t* dict_sources[SX_VECTOR_SIZE];
    if (!read_lengths(in, dict_count, dict_base, dict_width, dict_lengths)) {
        return false;
    }
    const uint8_t* bytes = sx_reader_take(in, dict_total);
    sx_reader_skip_to(in, 8);
    if (bytes == NULL || !split_bytes(bytes, dict_total, dict_lengths, dict_count, dict_sources)) {
        return false;
    }

    uint64_t codes[SX_VECTOR_SIZE];
    const uint8_t* packed = sx_reader_take(in, sx_bitpack_size(count, code_width));
    if (packed == NULL) {
        return false;
    }
    sx_bitunpack_u64(packed, count, code_width, codes);

    parsed->total_bytes = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (codes[i] >= dict_count) {
            return false;
        }
        parsed->sources[i] = dict_sources[codes[i]];
        parsed->lengths[i] = (uint32_t)dict_lengths[codes[i]];
        parsed->total_bytes += dict_lengths[codes[i]];
    }
    return true;
}

static sx_err parse_payload(sx_encoding encoding, const uint8_t* payload, size_t size,
                            uint32_t count, parsed_strings* parsed) {
    if (count == 0 || count > SX_VECTOR_SIZE) {
        return SX_ERR_INVALID_ARG;
    }
    sx_byte_reader in;
    sx_reader_init(&in, payload, size);

    bool ok = false;
    switch (encoding) {
        case SX_ENCODING_CONSTANT:
            ok = parse_constant(&in, count, parsed);
            break;
        case SX_ENCODING_STRING:
            ok = parse_string(&in, count, parsed);
            break;
        case SX_ENCODING_DICT:
            ok = parse_dict(&in, count, parsed);
            break;
        default:
            return SX_ERR_CORRUPT;
    }
    if (!ok || in.failed || sx_reader_remaining(&in) != 0 || parsed->total_bytes > UINT32_MAX) {
        return SX_ERR_CORRUPT;
    }
    return SX_OK;
}

sx_err sx_string_decoded_size(sx_encoding encoding, const uint8_t* payload, size_t size,
                              uint32_t count, size_t* data_bytes) {
    parsed_strings parsed;
    sx_err err = parse_payload(encoding, payload, size, count, &parsed);
    if (err == SX_OK) {
        *data_bytes = (size_t)parsed.total_bytes;
    }
    return err;
}

sx_err sx_string_decode(sx_encoding encoding, const uint8_t* payload, size_t size, uint32_t count,
                        uint32_t* offsets, uint8_t* data, size_t data_capacity) {
    parsed_strings parsed;
    sx_err err = parse_payload(encoding, payload, size, count, &parsed);
    if (err != SX_OK) {
        return err;
    }
    if (parsed.total_bytes > data_capacity) {
        return SX_ERR_LIMIT;
    }
    uint32_t position = 0;
    offsets[0] = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (parsed.lengths[i] > 0) {
            memcpy(data + position, parsed.sources[i], parsed.lengths[i]);
        }
        position += parsed.lengths[i];
        offsets[i + 1] = position;
    }
    return SX_OK;
}
