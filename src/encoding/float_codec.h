/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Encodings for double-precision chunks (DOUBLE; FLOAT columns are widened to double,
 * which is exact).
 *
 * ALP (Afroozeh, Kuffo, Boncz, SIGMOD 2024) targets doubles that originate from decimal
 * numbers, such as prices and sensor readings. Each value v is turned into an integer
 * d = round(v * 10^e * 10^-f) for an exponent e and factor f chosen per chunk, and is
 * kept only if d * 10^f * 10^-e reproduces v bit for bit. Values that do not survive the
 * round trip (NaN, infinities, -0.0, too many digits) are stored verbatim as exceptions.
 * The integers are then stored with frame-of-reference bit-packing.
 *
 * ALP_RD targets doubles with many significant digits. It splits each value's 64 bits
 * into a high "left" part (at most 16 bits), which usually takes very few distinct
 * values and is dictionary-encoded, and a "right" part, which is bit-packed as is.
 * Low bits that are zero in every value (29 of them for FLOAT values widened to double)
 * are shifted out of the right parts before packing.
 *
 * Payload layouts (little-endian):
 *
 *   RAW       f64 values[count]
 *   CONSTANT  u64 bits
 *   ALP       u8 exponent | u8 factor | u16 exception_count | pad[4]
 *             | i64 base | u8 width | pad[7] | packed[count]
 *             | u16 exception_positions[k] | pad to 8 | f64 exception_values[k]
 *   ALP_RD    u8 right_width | u8 dict_size | u8 code_width | u8 right_shift
 *             | u16 exception_count | pad[2] | u16 dict[8]
 *             | packed (right >> right_shift)[count] | packed codes[count]
 *             | u16 exception_positions[k] | u16 exception_left_parts[k] | pad to 8
 */

#ifndef SX_ENCODING_FLOAT_CODEC_H
#define SX_ENCODING_FLOAT_CODEC_H

#include "common/bytes.h"
#include "common/error.h"
#include "encoding/encoding.h"

#include <stddef.h>
#include <stdint.h>

/* Number of exponent/factor pairs an encoder remembers between chunks. */
#define SX_ALP_CANDIDATES 5u

/*
 * State carried across the chunks of one column while they are encoded in order.
 *
 * Choosing ALP parameters means trying all 190 exponent/factor pairs on a sample, which
 * dominates the encoding time of small chunks. Successive chunks of a column almost
 * always want the same few pairs, so the encoder remembers the best ones and tries only
 * those. It repeats the full search every 64 chunks, and immediately when the remembered
 * pairs produce clearly more exceptions than before (the data changed). The state only
 * affects how long parameter choice takes and never the correctness of the output.
 */
typedef struct sx_double_encoder {
    uint8_t exponents[SX_ALP_CANDIDATES];
    uint8_t factors[SX_ALP_CANDIDATES];
    uint32_t candidate_count;
    uint32_t search_exceptions;   /* sample exceptions of the best pair at the last search */
    uint32_t rd_right_width;      /* best ALP_RD split position at the last search */
    uint32_t chunks_until_search; /* chunks left before the next full search */
} sx_double_encoder;

/* Largest payload any chosen encoding can produce for `count` values. */
size_t sx_double_encode_bound(uint32_t count);

void sx_double_encoder_init(sx_double_encoder* encoder);

/*
 * Encodes 1 to SX_VECTOR_SIZE values, choosing among CONSTANT, ALP, ALP_RD, and RAW,
 * and reports the choice. `out` needs at least sx_double_encode_bound(count) free bytes.
 * Every value round-trips bit for bit, including NaN payloads and -0.0.
 */
sx_err sx_double_encoder_encode(sx_double_encoder* encoder, const double* values, uint32_t count,
                                sx_byte_writer* out, sx_encoding* chosen);

/* Encodes a single chunk with a fresh encoder (always a full parameter search). */
sx_err sx_double_encode(const double* values, uint32_t count, sx_byte_writer* out,
                        sx_encoding* chosen);

/*
 * Encodes with a specific encoding. Returns SX_ERR_UNSUPPORTED if the encoding cannot
 * represent the values and SX_ERR_LIMIT if `out` is too small.
 */
sx_err sx_double_encode_as(sx_encoding encoding, const double* values, uint32_t count,
                           sx_byte_writer* out);

/* Decodes `count` values into `out`, which must hold SX_VECTOR_SIZE values. */
sx_err sx_double_decode(sx_encoding encoding, const uint8_t* data, size_t size, uint32_t count,
                        double* out);

#endif /* SX_ENCODING_FLOAT_CODEC_H */
