/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "common/varint.h"

size_t sx_varint_encode(uint64_t value, uint8_t* dst) {
    size_t used = 0;
    while (value >= 0x80u) {
        dst[used] = (uint8_t)(value | 0x80u);
        value >>= 7;
        used++;
    }
    dst[used] = (uint8_t)value;
    return used + 1;
}

size_t sx_varint_decode(const uint8_t* src, size_t size, uint64_t* value) {
    uint64_t result = 0;
    for (size_t i = 0; i < size && i < SX_VARINT_MAX_BYTES; i++) {
        uint64_t byte = src[i];
        uint32_t shift = (uint32_t)(7 * i);
        /* The tenth byte may only contribute the single remaining bit. */
        if (i == SX_VARINT_MAX_BYTES - 1 && byte > 1u) {
            return 0;
        }
        result |= (byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            *value = result;
            return i + 1;
        }
    }
    return 0;
}
