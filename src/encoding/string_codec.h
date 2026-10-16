/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Encodings for VARCHAR chunks.
 *
 * Strings use the Arrow layout: a byte array plus `count + 1` offsets, where string i is
 * data[offsets[i] .. offsets[i + 1]). Decoding produces the same layout.
 *
 * Payload layouts (little-endian):
 *
 *   CONSTANT  u32 length | pad[4] | bytes[length] | pad to 8
 *   STRING    u32 total_bytes | u32 length_base | u8 length_width | pad[7]
 *             | packed lengths[count] | bytes[total_bytes] | pad to 8
 *   DICT      u16 dict_count | u8 code_width | pad[1] | u32 dict_total_bytes
 *             | u32 dict_length_base | u8 dict_length_width | pad[3]
 *             | packed dict lengths[dict_count] | dict bytes | pad to 8
 *             | packed codes[count]
 *
 * Lengths are stored as base + packed offset. DICT keeps distinct strings in order of
 * first appearance and stores one code per value.
 */

#ifndef SX_ENCODING_STRING_CODEC_H
#define SX_ENCODING_STRING_CODEC_H

#include "common/bytes.h"
#include "common/error.h"
#include "encoding/encoding.h"

#include <stddef.h>
#include <stdint.h>

/* Largest payload any chosen encoding can produce for these strings. */
size_t sx_string_encode_bound(const uint32_t* offsets, uint32_t count);

/* Encodes 1 to SX_VECTOR_SIZE strings with the smallest of CONSTANT, DICT, and STRING. */
sx_err sx_string_encode(const uint32_t* offsets, const uint8_t* data, uint32_t count,
                        sx_byte_writer* out, sx_encoding* chosen);

/* Encodes with a specific encoding; SX_ERR_UNSUPPORTED if it cannot represent the values. */
sx_err sx_string_encode_as(sx_encoding encoding, const uint32_t* offsets, const uint8_t* data,
                           uint32_t count, sx_byte_writer* out);

/* Total bytes of string data that decoding will produce, for sizing the output buffer. */
sx_err sx_string_decoded_size(sx_encoding encoding, const uint8_t* payload, size_t size,
                              uint32_t count, size_t* data_bytes);

/*
 * Decodes into `offsets` (count + 1 entries) and `data` (`data_capacity` bytes, at least
 * the size reported by sx_string_decoded_size).
 */
sx_err sx_string_decode(sx_encoding encoding, const uint8_t* payload, size_t size, uint32_t count,
                        uint32_t* offsets, uint8_t* data, size_t data_capacity);

#endif /* SX_ENCODING_STRING_CODEC_H */
