/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "common/parse.h"

#include <stdbool.h>
#include <stddef.h>

/* Returns the shift for a unit letter, or -1 if the letter is not a unit. */
static int unit_shift(char unit) {
    switch (unit) {
        case 'K':
        case 'k':
            return 10;
        case 'M':
        case 'm':
            return 20;
        case 'G':
        case 'g':
            return 30;
        case 'T':
        case 't':
            return 40;
        default:
            return -1;
    }
}

sx_err sx_parse_size(const char* text, uint64_t* bytes) {
    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return SX_ERR_INVALID_ARG;
    }

    uint64_t value = 0;
    size_t pos = 0;
    while (text[pos] >= '0' && text[pos] <= '9') {
        uint64_t digit = (uint64_t)(text[pos] - '0');
        if (value > (UINT64_MAX - digit) / 10u) {
            return SX_ERR_INVALID_ARG;
        }
        value = value * 10u + digit;
        pos++;
    }

    if (text[pos] == '\0') {
        *bytes = value;
        return SX_OK;
    }

    int shift = unit_shift(text[pos]);
    if (shift < 0 || text[pos + 1] != '\0') {
        return SX_ERR_INVALID_ARG;
    }
    if (value > (UINT64_MAX >> shift)) {
        return SX_ERR_INVALID_ARG;
    }
    *bytes = value << shift;
    return SX_OK;
}
