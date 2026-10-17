/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Bit-packing of unsigned 64-bit values in an interleaved layout (after FastLanes).
 *
 * A packed block has 16 lanes. Each lane packs its values one after another into 64-bit
 * words, and word k of lane l is stored at index (k * 16 + l). The values at the same
 * position ("row") in the 16 lanes therefore sit in 16 consecutive words, and unpacking
 * one row applies the same shift and mask to all of them: one or two SIMD operations.
 * A block of `count` values at `width` bits holds ceil(count / 16) rows; rows are padded
 * with zero values, so unpacking always produces a multiple of 16 values.
 *
 * Values are assigned to lanes in one of two orders:
 *
 *   interleaved      value i is in lane i % 16, row i / 16. Unpacking yields the values
 *                    in their original order. Used by every encoding except the delta
 *                    ones.
 *   lane-contiguous  value i is in lane i / rows, row i % rows, so each lane holds a run
 *                    of consecutive values. Used for the deltas of DELTA_FOR and
 *                    DELTA_PATCHED: the 16 lanes then compute independent prefix sums
 *                    (see kernels.h, rebuild_deltas). Sizes are the same in both orders.
 *
 * The packing loops themselves are kernels (kernels.h); the functions here pick the
 * active implementation.
 */

#ifndef SX_ENCODING_BITPACK_H
#define SX_ENCODING_BITPACK_H

#include <stddef.h>
#include <stdint.h>

#define SX_BITPACK_LANES     16u
#define SX_BITPACK_ROW_BYTES ((size_t)SX_BITPACK_LANES * sizeof(uint64_t))

/* Number of rows holding `count` values. */
static inline uint32_t sx_bitpack_rows(uint32_t count) {
    return (count + SX_BITPACK_LANES - 1u) / SX_BITPACK_LANES;
}

/* Number of values produced when unpacking `count` values: count rounded up to 16. */
static inline uint32_t sx_bitpack_padded_count(uint32_t count) {
    return sx_bitpack_rows(count) * SX_BITPACK_LANES;
}

/*
 * Position in unpacked (interleaved) order of value `index` of the lane-contiguous order,
 * for a block of `rows` rows.
 */
static inline size_t sx_bitpack_lane_position(uint32_t index, uint32_t rows) {
    return (size_t)(index % rows) * SX_BITPACK_LANES + index / rows;
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

/*
 * Reorders `count` values into the lane-contiguous order: afterwards packing `out` (as
 * sx_bitpack_padded_count(count) values) stores value i in lane i / rows, row i % rows.
 * Padding positions are set to zero.
 */
void sx_bitpack_to_lane_order(const uint64_t* values, uint32_t count, uint64_t* out);

#endif /* SX_ENCODING_BITPACK_H */
