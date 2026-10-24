/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "encoding/bitpack.h"

#include "encoding/kernels.h"

void sx_bitpack_u64(const uint64_t* values, uint32_t count, uint32_t width, uint8_t* out) {
    sx_codec_kernels_active()->bitpack(values, count, width, 0, out);
}

void sx_bitunpack_u64(const uint8_t* in, uint32_t count, uint32_t width, uint64_t* out) {
    sx_codec_kernels_active()->bitunpack(in, count, width, out);
}

void sx_bitpack_to_lane_order(const uint64_t* values, uint32_t count, uint64_t* out) {
    sx_codec_kernels_active()->to_lane_order(values, count, out);
}
