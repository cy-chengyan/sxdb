/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "encoding/bool_codec.h"

#include "common/bits.h"
#include "common/compiler.h"

#include <stdbool.h>

#define CONSTANT_SIZE    8u
#define CONSTANT_PADDING 7u

static size_t bitmap_size(uint32_t count) {
    return sx_div_round_up(count, 64) * sizeof(uint64_t);
}

size_t sx_bool_encode_bound(uint32_t count) {
    size_t bitmap = bitmap_size(count);
    return bitmap > CONSTANT_SIZE ? bitmap : CONSTANT_SIZE;
}

static bool all_same(const uint8_t* values, uint32_t count) {
    bool first = values[0] != 0;
    for (uint32_t i = 1; i < count; i++) {
        if ((values[i] != 0) != first) {
            return false;
        }
    }
    return true;
}

sx_err sx_bool_encode(const uint8_t* values, uint32_t count, sx_byte_writer* out,
                      sx_encoding* chosen) {
    if (count == 0 || count > SX_VECTOR_SIZE) {
        return SX_ERR_INVALID_ARG;
    }
    size_t start = out->size;

    if (all_same(values, count)) {
        sx_writer_put_u8(out, values[0] != 0 ? 1 : 0);
        sx_writer_reserve_zeroed(out, CONSTANT_PADDING);
        *chosen = SX_ENCODING_CONSTANT;
    } else {
        uint64_t words[SX_VECTOR_SIZE / 64] = {0};
        for (uint32_t i = 0; i < count; i++) {
            if (values[i] != 0) {
                words[i / 64] |= UINT64_C(1) << (i % 64);
            }
        }
        sx_writer_put_bytes(out, words, bitmap_size(count));
        *chosen = SX_ENCODING_BITMAP;
    }

    if (out->overflow) {
        out->size = start;
        out->overflow = false;
        return SX_ERR_LIMIT;
    }
    return SX_OK;
}

sx_err sx_bool_decode(sx_encoding encoding, const uint8_t* data, size_t size, uint32_t count,
                      uint8_t* out) {
    if (count == 0 || count > SX_VECTOR_SIZE) {
        return SX_ERR_INVALID_ARG;
    }
    sx_byte_reader in;
    sx_reader_init(&in, data, size);

    if (encoding == SX_ENCODING_CONSTANT) {
        uint8_t value = sx_reader_get_u8(&in) != 0 ? 1 : 0;
        sx_reader_take(&in, CONSTANT_PADDING);
        for (uint32_t i = 0; i < count; i++) {
            out[i] = value;
        }
    } else if (encoding == SX_ENCODING_BITMAP) {
        const uint8_t* words = sx_reader_take(&in, bitmap_size(count));
        if (words != NULL) {
            for (uint32_t i = 0; i < count; i++) {
                uint64_t word = sx_load_u64(words + (i / 64) * sizeof(uint64_t));
                out[i] = (uint8_t)((word >> (i % 64)) & 1u);
            }
        }
    } else {
        return SX_ERR_CORRUPT;
    }

    if (in.failed || sx_reader_remaining(&in) != 0) {
        return SX_ERR_CORRUPT;
    }
    return SX_OK;
}
