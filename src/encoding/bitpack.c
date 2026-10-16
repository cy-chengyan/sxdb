/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "encoding/bitpack.h"

#include "common/bits.h"

#include <string.h>

#define WORD_BYTES 8u

static uint32_t row_count(uint32_t count) {
    return (count + SX_BITPACK_LANES - 1u) / SX_BITPACK_LANES;
}

/* Number of 64-bit words each lane needs to hold `rows` values of `width` bits. */
static size_t words_per_lane(uint32_t rows, uint32_t width) {
    return sx_div_round_up((size_t)rows * width, 64);
}

size_t sx_bitpack_size(uint32_t count, uint32_t width) {
    return words_per_lane(row_count(count), width) * SX_BITPACK_LANES * WORD_BYTES;
}

/* ORs `bits` into the word at index `word_index` of the packed output. */
static void or_word(uint8_t* out, size_t word_index, uint64_t bits) {
    uint8_t* word = out + word_index * WORD_BYTES;
    sx_store_u64(word, sx_load_u64(word) | bits);
}

void sx_bitpack_u64(const uint64_t* values, uint32_t count, uint32_t width, uint8_t* out) {
    uint32_t rows = row_count(count);
    memset(out, 0, sx_bitpack_size(count, width));
    if (width == 0) {
        return;
    }
    uint64_t mask = sx_low_mask_u64(width);

    for (uint32_t lane = 0; lane < SX_BITPACK_LANES; lane++) {
        uint64_t bit_position = 0;
        for (uint32_t row = 0; row < rows; row++) {
            uint32_t index = row * SX_BITPACK_LANES + lane;
            uint64_t value = index < count ? (values[index] & mask) : 0;
            size_t word = (size_t)(bit_position / 64u);
            uint32_t offset = (uint32_t)(bit_position % 64u);

            or_word(out, word * SX_BITPACK_LANES + lane, value << offset);
            if (offset + width > 64u) {
                or_word(out, (word + 1u) * SX_BITPACK_LANES + lane, value >> (64u - offset));
            }
            bit_position += width;
        }
    }
}

void sx_bitunpack_u64(const uint8_t* in, uint32_t count, uint32_t width, uint64_t* out) {
    uint32_t rows = row_count(count);
    if (width == 0) {
        memset(out, 0, (size_t)rows * SX_BITPACK_LANES * sizeof(uint64_t));
        return;
    }
    uint64_t mask = sx_low_mask_u64(width);

    for (uint32_t row = 0; row < rows; row++) {
        uint64_t bit_position = (uint64_t)row * width;
        size_t word = (size_t)(bit_position / 64u);
        uint32_t offset = (uint32_t)(bit_position % 64u);
        const uint8_t* low_words = in + word * SX_BITPACK_LANES * WORD_BYTES;
        uint64_t* dst = out + (size_t)row * SX_BITPACK_LANES;

        if (offset + width <= 64u) {
            /* The value lies within one word in every lane. */
            for (uint32_t lane = 0; lane < SX_BITPACK_LANES; lane++) {
                uint64_t low = sx_load_u64(low_words + lane * WORD_BYTES);
                dst[lane] = (low >> offset) & mask;
            }
        } else {
            /* The value spans two consecutive words in every lane. */
            const uint8_t* high_words = low_words + SX_BITPACK_LANES * WORD_BYTES;
            uint32_t high_shift = 64u - offset;
            for (uint32_t lane = 0; lane < SX_BITPACK_LANES; lane++) {
                uint64_t low = sx_load_u64(low_words + lane * WORD_BYTES) >> offset;
                uint64_t high = sx_load_u64(high_words + lane * WORD_BYTES) << high_shift;
                dst[lane] = (low | high) & mask;
            }
        }
    }
}
