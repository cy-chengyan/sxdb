/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "encoding/bitpack.h"

#include "common/bits.h"
#include "encoding/kernels.h"

#include <string.h>

size_t sx_bitpack_size(uint32_t count, uint32_t width) {
    size_t words_per_lane = sx_div_round_up((size_t)sx_bitpack_rows(count) * width, 64);
    return words_per_lane * SX_BITPACK_ROW_BYTES;
}

void sx_bitpack_u64(const uint64_t* values, uint32_t count, uint32_t width, uint8_t* out) {
    sx_codec_kernels_active()->bitpack(values, count, width, 0, out);
}

void sx_bitunpack_u64(const uint8_t* in, uint32_t count, uint32_t width, uint64_t* out) {
    sx_codec_kernels_active()->bitunpack(in, count, width, out);
}

void sx_bitpack_to_lane_order(const uint64_t* values, uint32_t count, uint64_t* out) {
    uint32_t rows = sx_bitpack_rows(count);
    memset(out, 0, (size_t)rows * SX_BITPACK_ROW_BYTES);
    /* Lane l takes the run of values starting at l * rows. */
    for (uint32_t lane = 0; lane < SX_BITPACK_LANES && lane * rows < count; lane++) {
        const uint64_t* run = values + (size_t)lane * rows;
        uint32_t run_length = count - lane * rows < rows ? count - lane * rows : rows;
        for (uint32_t row = 0; row < run_length; row++) {
            out[(size_t)row * SX_BITPACK_LANES + lane] = run[row];
        }
    }
}
