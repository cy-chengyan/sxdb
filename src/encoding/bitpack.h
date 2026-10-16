/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Bit-packing of unsigned 64-bit values in an interleaved layout (after FastLanes).
 *
 * Values are laid out in 16 lanes: value i belongs to lane (i % 16), row (i / 16).
 * Each lane packs its rows one after another into 64-bit words, and word k of lane l is
 * stored at index (k * 16 + l). Unpacking one row therefore applies the same shift and
 * mask to 16 consecutive words, a loop compilers vectorize on any SIMD width without
 * hand-written intrinsics.
 *
 * A packed block of `count` values at `width` bits holds ceil(count / 16) rows. Rows are
 * padded with zero values, so unpacking always produces a multiple of 16 values.
 */

#ifndef SX_ENCODING_BITPACK_H
#define SX_ENCODING_BITPACK_H

#include <stddef.h>
#include <stdint.h>

#define SX_BITPACK_LANES 16u

/* Number of values produced when unpacking `count` values: count rounded up to 16. */
static inline uint32_t sx_bitpack_padded_count(uint32_t count) {
    return (count + SX_BITPACK_LANES - 1u) / SX_BITPACK_LANES * SX_BITPACK_LANES;
}

/* Size in bytes of `count` values packed at `width` bits (0 to 64). */
size_t sx_bitpack_size(uint32_t count, uint32_t width);

/*
 * Packs the low `width` bits of each of the `count` values into `out`, which must hold
 * sx_bitpack_size(count, width) bytes.
 */
void sx_bitpack_u64(const uint64_t* values, uint32_t count, uint32_t width, uint8_t* out);

/*
 * Unpacks `count` values of `width` bits from `in` into `out`. Writes
 * sx_bitpack_padded_count(count) values; the padding values are zero.
 */
void sx_bitunpack_u64(const uint8_t* in, uint32_t count, uint32_t width, uint64_t* out);

#endif /* SX_ENCODING_BITPACK_H */
