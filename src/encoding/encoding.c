/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "encoding/encoding.h"

const char* sx_encoding_name(sx_encoding encoding) {
    switch (encoding) {
        case SX_ENCODING_RAW:
            return "RAW";
        case SX_ENCODING_CONSTANT:
            return "CONSTANT";
        case SX_ENCODING_RLE:
            return "RLE";
        case SX_ENCODING_FOR_BITPACK:
            return "FOR_BITPACK";
        case SX_ENCODING_DELTA_FOR:
            return "DELTA_FOR";
        case SX_ENCODING_DELTA_CONST:
            return "DELTA_CONST";
        case SX_ENCODING_ALP:
            return "ALP";
        case SX_ENCODING_ALP_RD:
            return "ALP_RD";
        case SX_ENCODING_DICT:
            return "DICT";
        case SX_ENCODING_STRING:
            return "STRING";
        case SX_ENCODING_BITMAP:
            return "BITMAP";
    }
    return "UNKNOWN";
}
