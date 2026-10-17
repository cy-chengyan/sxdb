/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Encodings for 64-bit integer chunks: timestamps, INT, BIGINT, DECIMAL (scaled
 * integers), series ids, and dictionary ids.
 *
 * Payload layouts (little-endian, offsets relative to the payload start):
 *
 *   RAW          i64 values[count]
 *   CONSTANT     i64 value
 *   DELTA_CONST  i64 first | i64 step
 *   RLE          u16 run_count | pad[6] | i64 run_values[run_count]
 *                | u16 run_lengths[run_count] | pad to 8
 *   FOR_BITPACK  i64 base | u64 multiplier | u8 width | pad[7] | packed[count]
 *   DELTA_FOR    i64 first | i64 delta_base | u64 multiplier | u8 width | pad[7]
 *                | packed[count - 1]
 *   DELTA_PATCHED
 *                i64 first | i64 delta_base | u64 multiplier | u8 width | pad[1]
 *                | u16 exception_count | pad[4] | packed[count - 1]
 *                | u16 exception_positions[k] | pad to 8 | u64 exception_offsets[k]
 *
 * Decoding:
 *
 *   DELTA_CONST  value[i] = first + i * step
 *   FOR_BITPACK  value[i] = base + packed[i] * multiplier
 *   DELTA_FOR    value[0] = first
 *                value[i] = value[i - 1] + delta_base + offset[i - 1] * multiplier
 *                with offset = packed
 *   DELTA_PATCHED
 *                as DELTA_FOR, except that offset[exception_positions[j]] is
 *                exception_offsets[j] (its packed slot holds 0)
 *
 * DELTA_PATCHED serves timestamps with a few large gaps, such as trading sessions with
 * a lunch break or overnight gaps: the regular steps are packed narrowly and the few
 * large ones are stored apart instead of widening every value.
 *
 * The multiplier is the greatest common divisor of the offsets, so data whose values or
 * steps share a factor (millisecond timestamps stored as nanoseconds, prices that are
 * multiples of 100) costs no extra bits. All arithmetic wraps modulo 2^64, so extreme
 * values and overflowing differences round-trip exactly.
 */

#ifndef SX_ENCODING_INT_CODEC_H
#define SX_ENCODING_INT_CODEC_H

#include "common/bytes.h"
#include "common/error.h"
#include "encoding/encoding.h"

#include <stddef.h>
#include <stdint.h>

/* Largest payload any chosen encoding can produce for `count` values. */
size_t sx_int64_encode_bound(uint32_t count);

/*
 * Encodes 1 to SX_VECTOR_SIZE values with the smallest applicable encoding and reports
 * which one was chosen. `out` needs at least sx_int64_encode_bound(count) free bytes.
 */
sx_err sx_int64_encode(const int64_t* values, uint32_t count, sx_byte_writer* out,
                       sx_encoding* chosen);

/*
 * Encodes with a specific encoding. Returns SX_ERR_UNSUPPORTED if the encoding cannot
 * represent the values (CONSTANT for differing values, DELTA_CONST for irregular steps)
 * and SX_ERR_LIMIT if `out` is too small.
 */
sx_err sx_int64_encode_as(sx_encoding encoding, const int64_t* values, uint32_t count,
                          sx_byte_writer* out);

/* Decodes `count` values into `out`, which must hold SX_VECTOR_SIZE values. */
sx_err sx_int64_decode(sx_encoding encoding, const uint8_t* data, size_t size, uint32_t count,
                       int64_t* out);

#endif /* SX_ENCODING_INT_CODEC_H */
