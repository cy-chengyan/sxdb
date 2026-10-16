/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Encodings for BOOL chunks. Values are bytes: 0 is false, anything else is true;
 * decoding produces 0 and 1.
 *
 * Payload layouts (little-endian):
 *
 *   CONSTANT  u8 value | pad[7]
 *   BITMAP    u64 words[ceil(count / 64)]     bit (i % 64) of word (i / 64) is value i
 */

#ifndef SX_ENCODING_BOOL_CODEC_H
#define SX_ENCODING_BOOL_CODEC_H

#include "common/bytes.h"
#include "common/error.h"
#include "encoding/encoding.h"

#include <stddef.h>
#include <stdint.h>

size_t sx_bool_encode_bound(uint32_t count);

sx_err sx_bool_encode(const uint8_t* values, uint32_t count, sx_byte_writer* out,
                      sx_encoding* chosen);

/* Decodes `count` values (0 or 1) into `out`, which must hold SX_VECTOR_SIZE bytes. */
sx_err sx_bool_decode(sx_encoding encoding, const uint8_t* data, size_t size, uint32_t count,
                      uint8_t* out);

#endif /* SX_ENCODING_BOOL_CODEC_H */
