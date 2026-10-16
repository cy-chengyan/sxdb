/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "encoding/float_codec.h"

#include "common/bits.h"
#include "common/compiler.h"
#include "encoding/bitpack.h"

#include <stdbool.h>
#include <string.h>

/* Number of values examined when choosing encoding parameters. */
#define SAMPLE_SIZE 32u

/* ---------------------------------------------------------------------------------------- */
/* ALP parameters                                                                           */
/* ---------------------------------------------------------------------------------------- */

#define ALP_MAX_EXPONENT 18
#define ALP_HEADER_SIZE  24u
#define ALP_HEADER_PAD_1 4u
#define ALP_HEADER_PAD_2 7u

/* Bits needed to store one ALP exception: a u16 position and the f64 value. */
#define ALP_EXCEPTION_BITS 80u

/*
 * Rounding by adding and subtracting 2^52 + 2^51 is exact round-to-nearest for
 * |x| < 2^51; larger values are treated as exceptions.
 */
#define ALP_ROUND_MAGIC 6755399441055744.0
#define ALP_ROUND_LIMIT 2251799813685248.0

static const double POW10[ALP_MAX_EXPONENT + 1] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18,
};

static const double NEG_POW10[ALP_MAX_EXPONENT + 1] = {
    1e0,   1e-1,  1e-2,  1e-3,  1e-4,  1e-5,  1e-6,  1e-7,  1e-8,  1e-9,
    1e-10, 1e-11, 1e-12, 1e-13, 1e-14, 1e-15, 1e-16, 1e-17, 1e-18,
};

/*
 * The decoding formula. The encoder uses this same function to verify every value, so
 * encoder and decoder can never disagree.
 */
static inline double alp_decode_value(int64_t digits, int exponent, int factor) {
    return (double)digits * POW10[factor] * NEG_POW10[exponent];
}

/* Encodes one value; returns false if it does not round-trip exactly. */
static bool alp_encode_value(double value, int exponent, int factor, int64_t* digits) {
    double scaled = value * POW10[exponent] * NEG_POW10[factor];
    if (!(scaled > -ALP_ROUND_LIMIT && scaled < ALP_ROUND_LIMIT)) {
        return false;
    }
    double rounded = (scaled + ALP_ROUND_MAGIC) - ALP_ROUND_MAGIC;
    int64_t candidate = (int64_t)rounded;
    double decoded = alp_decode_value(candidate, exponent, factor);
    if (sx_double_to_bits(decoded) != sx_double_to_bits(value)) {
        return false;
    }
    *digits = candidate;
    return true;
}

/* ---------------------------------------------------------------------------------------- */
/* ALP_RD parameters                                                                        */
/* ---------------------------------------------------------------------------------------- */

#define RD_MAX_DICT        8u
#define RD_MAX_CODE_WIDTH  3u
#define RD_MIN_RIGHT_WIDTH 48u
#define RD_MAX_RIGHT_WIDTH 63u
#define RD_HEADER_SIZE     24u

/* Bits needed to store one ALP_RD exception: a u16 position and a u16 left part. */
#define RD_EXCEPTION_BITS 32u

typedef struct rd_dictionary {
    uint16_t entries[RD_MAX_DICT];
    uint32_t size;
} rd_dictionary;

/* ---------------------------------------------------------------------------------------- */
/* Sampling and parameter choice                                                            */
/* ---------------------------------------------------------------------------------------- */

typedef struct sample {
    double values[SAMPLE_SIZE];
    uint32_t size;
} sample;

/* Takes up to SAMPLE_SIZE values spread evenly over the chunk. */
static void take_sample(const double* values, uint32_t count, sample* out) {
    uint32_t step = count > SAMPLE_SIZE ? count / SAMPLE_SIZE : 1u;
    out->size = 0;
    for (uint32_t i = 0; i < count && out->size < SAMPLE_SIZE; i += step) {
        out->values[out->size] = values[i];
        out->size++;
    }
}

/* Scales a per-sample count up to the whole chunk, rounding up. */
static uint64_t scale_to_chunk(uint32_t sample_count, uint32_t sample_size, uint32_t count) {
    return ((uint64_t)sample_count * count + sample_size - 1u) / sample_size;
}

typedef struct alp_choice {
    int exponent;
    int factor;
    uint64_t estimated_bits;
} alp_choice;

/* Tries every exponent/factor pair on the sample and keeps the cheapest. */
static alp_choice choose_alp_parameters(const sample* sampled, uint32_t count) {
    alp_choice best = {.exponent = 0, .factor = 0, .estimated_bits = UINT64_MAX};
    for (int exponent = 0; exponent <= ALP_MAX_EXPONENT; exponent++) {
        for (int factor = 0; factor <= exponent; factor++) {
            uint32_t exceptions = 0;
            bool any_encoded = false;
            int64_t min_digits = 0;
            int64_t max_digits = 0;
            for (uint32_t i = 0; i < sampled->size; i++) {
                int64_t digits = 0;
                if (!alp_encode_value(sampled->values[i], exponent, factor, &digits)) {
                    exceptions++;
                } else if (!any_encoded) {
                    min_digits = digits;
                    max_digits = digits;
                    any_encoded = true;
                } else if (digits < min_digits) {
                    min_digits = digits;
                } else if (digits > max_digits) {
                    max_digits = digits;
                }
            }
            uint32_t width = sx_bit_width_u64((uint64_t)max_digits - (uint64_t)min_digits);
            uint64_t bits = (uint64_t)count * width +
                            scale_to_chunk(exceptions, sampled->size, count) * ALP_EXCEPTION_BITS;
            if (bits < best.estimated_bits) {
                best.exponent = exponent;
                best.factor = factor;
                best.estimated_bits = bits;
            }
        }
    }
    return best;
}

/*
 * Adds `value` with `frequency` to a dictionary kept in descending frequency order,
 * displacing the least frequent entry when the dictionary is full.
 */
static void rd_dictionary_offer(rd_dictionary* dict, uint32_t* frequencies, uint16_t value,
                                uint32_t frequency) {
    uint32_t slot = dict->size;
    if (dict->size == RD_MAX_DICT) {
        slot = RD_MAX_DICT - 1u;
        if (frequency <= frequencies[slot]) {
            return;
        }
    } else {
        dict->size++;
    }
    while (slot > 0 && frequencies[slot - 1] < frequency) {
        frequencies[slot] = frequencies[slot - 1];
        dict->entries[slot] = dict->entries[slot - 1];
        slot--;
    }
    frequencies[slot] = frequency;
    dict->entries[slot] = value;
}

/* Hash table size for counting left parts: a power of two, at least twice the values. */
#define RD_COUNT_TABLE_MAX (2u * SX_VECTOR_SIZE)

/* Builds the dictionary of the (up to 8) most frequent left parts. */
static void build_rd_dictionary(const uint16_t* left_parts, uint32_t count, rd_dictionary* dict) {
    uint32_t table_size = 64;
    while (table_size < 2u * count) {
        table_size *= 2u;
    }
    uint32_t mask = table_size - 1u;
    uint16_t keys[RD_COUNT_TABLE_MAX];
    uint32_t counts[RD_COUNT_TABLE_MAX];
    memset(counts, 0, table_size * sizeof(uint32_t));

    /* Count occurrences with open addressing; a zero count marks an empty slot. */
    for (uint32_t i = 0; i < count; i++) {
        uint16_t left = left_parts[i];
        uint32_t slot = ((uint32_t)left * 40503u) & mask;
        while (counts[slot] != 0 && keys[slot] != left) {
            slot = (slot + 1u) & mask;
        }
        keys[slot] = left;
        counts[slot]++;
    }

    uint32_t frequencies[RD_MAX_DICT] = {0};
    dict->size = 0;
    for (uint32_t slot = 0; slot < table_size; slot++) {
        if (counts[slot] != 0) {
            rd_dictionary_offer(dict, frequencies, keys[slot], counts[slot]);
        }
    }
}

/* Index of `left` in the dictionary, or -1 if it is not there. */
static int rd_dictionary_find(const rd_dictionary* dict, uint16_t left) {
    for (uint32_t i = 0; i < dict->size; i++) {
        if (dict->entries[i] == left) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t rd_code_width(uint32_t dict_size) {
    return dict_size <= 1 ? 0 : sx_bit_width_u64(dict_size - 1u);
}

typedef struct rd_choice {
    uint32_t right_width;
    uint64_t estimated_bits;
} rd_choice;

/*
 * Number of low bits that are zero in every value, capped at `limit`. FLOAT values
 * widened to double, for example, always end in 29 zero bits, which need not be stored.
 */
static uint32_t common_trailing_zeros(const double* values, uint32_t count, uint32_t limit) {
    uint64_t all_bits = 0;
    for (uint32_t i = 0; i < count; i++) {
        all_bits |= sx_double_to_bits(values[i]);
    }
    if (all_bits == 0) {
        return limit;
    }
    uint32_t zeros = sx_trailing_zeros_u64(all_bits);
    return zeros < limit ? zeros : limit;
}

/* Tries every split position on the sample and keeps the cheapest. */
static rd_choice choose_rd_parameters(const sample* sampled, uint32_t count) {
    rd_choice best = {.right_width = RD_MAX_RIGHT_WIDTH, .estimated_bits = UINT64_MAX};
    for (uint32_t right_width = RD_MIN_RIGHT_WIDTH; right_width <= RD_MAX_RIGHT_WIDTH;
         right_width++) {
        uint32_t shift = common_trailing_zeros(sampled->values, sampled->size, right_width);
        uint16_t left_parts[SAMPLE_SIZE];
        for (uint32_t i = 0; i < sampled->size; i++) {
            left_parts[i] = (uint16_t)(sx_double_to_bits(sampled->values[i]) >> right_width);
        }
        rd_dictionary dict;
        build_rd_dictionary(left_parts, sampled->size, &dict);

        uint32_t exceptions = 0;
        for (uint32_t i = 0; i < sampled->size; i++) {
            if (rd_dictionary_find(&dict, left_parts[i]) < 0) {
                exceptions++;
            }
        }
        uint64_t bits_per_value = right_width - shift + rd_code_width(dict.size);
        uint64_t bits = (uint64_t)count * bits_per_value +
                        scale_to_chunk(exceptions, sampled->size, count) * RD_EXCEPTION_BITS;
        if (bits < best.estimated_bits) {
            best.right_width = right_width;
            best.estimated_bits = bits;
        }
    }
    return best;
}

/* ---------------------------------------------------------------------------------------- */
/* Encoders                                                                                 */
/* ---------------------------------------------------------------------------------------- */

static bool all_bits_equal(const double* values, uint32_t count) {
    uint64_t first = sx_double_to_bits(values[0]);
    for (uint32_t i = 1; i < count; i++) {
        if (sx_double_to_bits(values[i]) != first) {
            return false;
        }
    }
    return true;
}

static void write_packed(sx_byte_writer* out, const uint64_t* values, uint32_t count,
                         uint32_t width) {
    uint8_t* packed = sx_writer_reserve(out, sx_bitpack_size(count, width));
    if (packed != NULL) {
        sx_bitpack_u64(values, count, width, packed);
    }
}

static void encode_alp(const double* values, uint32_t count, int exponent, int factor,
                       sx_byte_writer* out) {
    int64_t digits[SX_VECTOR_SIZE];
    uint16_t exception_positions[SX_VECTOR_SIZE];
    uint32_t exception_count = 0;
    bool any_encoded = false;
    int64_t min_digits = 0;
    int64_t max_digits = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (!alp_encode_value(values[i], exponent, factor, &digits[i])) {
            exception_positions[exception_count] = (uint16_t)i;
            exception_count++;
            continue;
        }
        if (!any_encoded || digits[i] < min_digits) {
            min_digits = digits[i];
        }
        if (!any_encoded || digits[i] > max_digits) {
            max_digits = digits[i];
        }
        any_encoded = true;
    }

    /* Exceptions get the base as a placeholder so they do not widen the packed range. */
    for (uint32_t i = 0; i < exception_count; i++) {
        digits[exception_positions[i]] = min_digits;
    }

    uint64_t offsets[SX_VECTOR_SIZE];
    for (uint32_t i = 0; i < count; i++) {
        offsets[i] = (uint64_t)digits[i] - (uint64_t)min_digits;
    }
    uint32_t width = sx_bit_width_u64((uint64_t)max_digits - (uint64_t)min_digits);

    sx_writer_put_u8(out, (uint8_t)exponent);
    sx_writer_put_u8(out, (uint8_t)factor);
    sx_writer_put_u16(out, (uint16_t)exception_count);
    sx_writer_reserve_zeroed(out, ALP_HEADER_PAD_1);
    sx_writer_put_i64(out, min_digits);
    sx_writer_put_u8(out, (uint8_t)width);
    sx_writer_reserve_zeroed(out, ALP_HEADER_PAD_2);
    write_packed(out, offsets, count, width);

    sx_writer_put_bytes(out, exception_positions, (size_t)exception_count * sizeof(uint16_t));
    sx_writer_pad_to(out, 8);
    for (uint32_t i = 0; i < exception_count; i++) {
        sx_writer_put_f64(out, values[exception_positions[i]]);
    }
}

static void encode_alp_rd(const double* values, uint32_t count, uint32_t right_width,
                          sx_byte_writer* out) {
    /* Low bits that are zero in every value are shifted out of the right parts. */
    uint32_t right_shift = common_trailing_zeros(values, count, right_width);
    /* Zeroed so that GCC can see build_rd_dictionary never reads uninitialized entries. */
    uint16_t left_parts[SX_VECTOR_SIZE] = {0};
    uint64_t right_parts[SX_VECTOR_SIZE];
    for (uint32_t i = 0; i < count; i++) {
        uint64_t bits = sx_double_to_bits(values[i]);
        left_parts[i] = (uint16_t)(bits >> right_width);
        right_parts[i] = (bits & sx_low_mask_u64(right_width)) >> right_shift;
    }
    rd_dictionary dict;
    build_rd_dictionary(left_parts, count, &dict);

    uint64_t codes[SX_VECTOR_SIZE];
    uint16_t exception_positions[SX_VECTOR_SIZE];
    uint16_t exception_lefts[SX_VECTOR_SIZE];
    uint32_t exception_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        int code = rd_dictionary_find(&dict, left_parts[i]);
        if (code < 0) {
            codes[i] = 0;
            exception_positions[exception_count] = (uint16_t)i;
            exception_lefts[exception_count] = left_parts[i];
            exception_count++;
        } else {
            codes[i] = (uint64_t)code;
        }
    }
    uint32_t code_width = rd_code_width(dict.size);

    sx_writer_put_u8(out, (uint8_t)right_width);
    sx_writer_put_u8(out, (uint8_t)dict.size);
    sx_writer_put_u8(out, (uint8_t)code_width);
    sx_writer_put_u8(out, (uint8_t)right_shift);
    sx_writer_put_u16(out, (uint16_t)exception_count);
    sx_writer_put_u16(out, 0);
    for (uint32_t i = 0; i < RD_MAX_DICT; i++) {
        sx_writer_put_u16(out, i < dict.size ? dict.entries[i] : 0);
    }
    write_packed(out, right_parts, count, right_width - right_shift);
    write_packed(out, codes, count, code_width);
    sx_writer_put_bytes(out, exception_positions, (size_t)exception_count * sizeof(uint16_t));
    sx_writer_put_bytes(out, exception_lefts, (size_t)exception_count * sizeof(uint16_t));
    sx_writer_pad_to(out, 8);
}

/*
 * Writes one encoding. For ALP and ALP_RD the parameters are chosen from a sample.
 * Returns SX_ERR_UNSUPPORTED if the encoding does not apply, SX_ERR_LIMIT on overflow.
 */
static sx_err encode_with(sx_encoding encoding, const double* values, uint32_t count,
                          sx_byte_writer* out) {
    size_t start = out->size;
    sample sampled;
    switch (encoding) {
        case SX_ENCODING_RAW:
            sx_writer_put_bytes(out, values, (size_t)count * sizeof(double));
            break;
        case SX_ENCODING_CONSTANT:
            if (!all_bits_equal(values, count)) {
                return SX_ERR_UNSUPPORTED;
            }
            sx_writer_put_f64(out, values[0]);
            break;
        case SX_ENCODING_ALP: {
            take_sample(values, count, &sampled);
            alp_choice choice = choose_alp_parameters(&sampled, count);
            encode_alp(values, count, choice.exponent, choice.factor, out);
            break;
        }
        case SX_ENCODING_ALP_RD: {
            take_sample(values, count, &sampled);
            rd_choice choice = choose_rd_parameters(&sampled, count);
            encode_alp_rd(values, count, choice.right_width, out);
            break;
        }
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

size_t sx_double_encode_bound(uint32_t count) {
    /* Anything larger than RAW is replaced by RAW. */
    return (size_t)count * sizeof(double);
}

sx_err sx_double_encode(const double* values, uint32_t count, sx_byte_writer* out,
                        sx_encoding* chosen) {
    if (count == 0 || count > SX_VECTOR_SIZE) {
        return SX_ERR_INVALID_ARG;
    }
    if (all_bits_equal(values, count)) {
        *chosen = SX_ENCODING_CONSTANT;
        return encode_with(SX_ENCODING_CONSTANT, values, count, out);
    }

    sample sampled;
    take_sample(values, count, &sampled);
    alp_choice alp = choose_alp_parameters(&sampled, count);
    rd_choice rd = choose_rd_parameters(&sampled, count);
    uint64_t raw_bits = (uint64_t)count * 64u;

    sx_encoding candidate = SX_ENCODING_RAW;
    if (alp.estimated_bits <= rd.estimated_bits && alp.estimated_bits < raw_bits) {
        candidate = SX_ENCODING_ALP;
    } else if (rd.estimated_bits < raw_bits) {
        candidate = SX_ENCODING_ALP_RD;
    }

    /* The sample can underestimate exceptions; fall back to RAW if the result is larger. */
    size_t start = out->size;
    size_t raw_size = (size_t)count * sizeof(double);
    if (candidate == SX_ENCODING_ALP) {
        encode_alp(values, count, alp.exponent, alp.factor, out);
    } else if (candidate == SX_ENCODING_ALP_RD) {
        encode_alp_rd(values, count, rd.right_width, out);
    }
    if (candidate != SX_ENCODING_RAW && !out->overflow && out->size - start < raw_size) {
        *chosen = candidate;
        return SX_OK;
    }
    out->size = start;
    out->overflow = false;
    *chosen = SX_ENCODING_RAW;
    return encode_with(SX_ENCODING_RAW, values, count, out);
}

sx_err sx_double_encode_as(sx_encoding encoding, const double* values, uint32_t count,
                           sx_byte_writer* out) {
    if (count == 0 || count > SX_VECTOR_SIZE) {
        return SX_ERR_INVALID_ARG;
    }
    return encode_with(encoding, values, count, out);
}

/* ---------------------------------------------------------------------------------------- */
/* Decoders                                                                                 */
/* ---------------------------------------------------------------------------------------- */

static bool read_packed(sx_byte_reader* in, uint32_t count, uint32_t width, uint64_t* values) {
    if (width > 64) {
        return false;
    }
    const uint8_t* packed = sx_reader_take(in, sx_bitpack_size(count, width));
    if (packed == NULL) {
        return false;
    }
    sx_bitunpack_u64(packed, count, width, values);
    return true;
}

static bool decode_raw(sx_byte_reader* in, uint32_t count, double* out) {
    const uint8_t* src = sx_reader_take(in, (size_t)count * sizeof(double));
    if (src == NULL) {
        return false;
    }
    memcpy(out, src, (size_t)count * sizeof(double));
    return true;
}

static bool decode_constant(sx_byte_reader* in, uint32_t count, double* out) {
    double value = sx_reader_get_f64(in);
    for (uint32_t i = 0; i < count; i++) {
        out[i] = value;
    }
    return true;
}

static bool decode_alp(sx_byte_reader* in, uint32_t count, double* out) {
    int exponent = sx_reader_get_u8(in);
    int factor = sx_reader_get_u8(in);
    uint32_t exception_count = sx_reader_get_u16(in);
    sx_reader_take(in, ALP_HEADER_PAD_1);
    uint64_t base = sx_reader_get_u64(in);
    uint32_t width = sx_reader_get_u8(in);
    sx_reader_take(in, ALP_HEADER_PAD_2);
    if (exponent > ALP_MAX_EXPONENT || factor > exponent || exception_count > count) {
        return false;
    }

    uint64_t offsets[SX_VECTOR_SIZE];
    if (!read_packed(in, count, width, offsets)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        int64_t digits = (int64_t)(base + offsets[i]);
        out[i] = alp_decode_value(digits, exponent, factor);
    }

    const uint8_t* positions = sx_reader_take(in, (size_t)exception_count * sizeof(uint16_t));
    sx_reader_skip_to(in, 8);
    const uint8_t* exceptions = sx_reader_take(in, (size_t)exception_count * sizeof(double));
    if (positions == NULL || exceptions == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < exception_count; i++) {
        uint32_t position = sx_load_u16(positions + i * sizeof(uint16_t));
        if (position >= count) {
            return false;
        }
        out[position] = sx_bits_to_double(sx_load_u64(exceptions + i * sizeof(double)));
    }
    return true;
}

static bool decode_alp_rd(sx_byte_reader* in, uint32_t count, double* out) {
    uint32_t right_width = sx_reader_get_u8(in);
    uint32_t dict_size = sx_reader_get_u8(in);
    uint32_t code_width = sx_reader_get_u8(in);
    uint32_t right_shift = sx_reader_get_u8(in);
    uint32_t exception_count = sx_reader_get_u16(in);
    sx_reader_take(in, 2);
    uint64_t dict[RD_MAX_DICT];
    for (uint32_t i = 0; i < RD_MAX_DICT; i++) {
        dict[i] = sx_reader_get_u16(in);
    }
    if (right_width < RD_MIN_RIGHT_WIDTH || right_width > RD_MAX_RIGHT_WIDTH ||
        right_shift > right_width || dict_size == 0 || dict_size > RD_MAX_DICT ||
        code_width != rd_code_width(dict_size) || exception_count > count) {
        return false;
    }

    uint64_t right_parts[SX_VECTOR_SIZE];
    uint64_t codes[SX_VECTOR_SIZE];
    if (!read_packed(in, count, right_width - right_shift, right_parts) ||
        !read_packed(in, count, code_width, codes)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        right_parts[i] <<= right_shift;
    }
    /* code_width is at most 3, so every code indexes within the 8-entry dictionary. */
    for (uint32_t i = 0; i < count; i++) {
        uint64_t bits = (dict[codes[i]] << right_width) | right_parts[i];
        out[i] = sx_bits_to_double(bits);
    }

    const uint8_t* positions = sx_reader_take(in, (size_t)exception_count * sizeof(uint16_t));
    const uint8_t* lefts = sx_reader_take(in, (size_t)exception_count * sizeof(uint16_t));
    sx_reader_skip_to(in, 8);
    if (positions == NULL || lefts == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < exception_count; i++) {
        uint32_t position = sx_load_u16(positions + i * sizeof(uint16_t));
        if (position >= count) {
            return false;
        }
        uint64_t left = sx_load_u16(lefts + i * sizeof(uint16_t));
        out[position] = sx_bits_to_double((left << right_width) | right_parts[position]);
    }
    return true;
}

sx_err sx_double_decode(sx_encoding encoding, const uint8_t* data, size_t size, uint32_t count,
                        double* out) {
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
        case SX_ENCODING_ALP:
            ok = decode_alp(&in, count, out);
            break;
        case SX_ENCODING_ALP_RD:
            ok = decode_alp_rd(&in, count, out);
            break;
        default:
            return SX_ERR_CORRUPT;
    }
    if (!ok || in.failed || sx_reader_remaining(&in) != 0) {
        return SX_ERR_CORRUPT;
    }
    return SX_OK;
}
