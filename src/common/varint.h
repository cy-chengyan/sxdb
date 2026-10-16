/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Unsigned LEB128 variable-length integers: 7 bits per byte, low bits first, with the
 * high bit of each byte marking continuation. A 64-bit value takes 1 to 10 bytes.
 */

#ifndef SX_COMMON_VARINT_H
#define SX_COMMON_VARINT_H

#include <stddef.h>
#include <stdint.h>

#define SX_VARINT_MAX_BYTES 10u

/* Writes `value` to `dst`, which must have room for SX_VARINT_MAX_BYTES. Returns bytes used. */
size_t sx_varint_encode(uint64_t value, uint8_t* dst);

/*
 * Reads one varint from `src` (at most `size` bytes). Returns the number of bytes
 * consumed, or 0 if the input is truncated or longer than a 64-bit value allows.
 */
size_t sx_varint_decode(const uint8_t* src, size_t size, uint64_t* value);

#endif /* SX_COMMON_VARINT_H */
