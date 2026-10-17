/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Column encodings. See docs/DESIGN.md section 4.2.
 *
 * An encoding turns one chunk (1 to SX_VECTOR_SIZE values of one column) into a
 * payload. The storage layer records the encoding id and the value count in the chunk
 * header, so payloads do not repeat them.
 *
 * The numeric ids are stored on disk and must never change.
 *
 * Decoders treat their input as untrusted: they validate every length and field and
 * return SX_ERR_CORRUPT instead of reading or writing out of bounds. Decoders write
 * whole groups of 16 values, so output arrays must hold SX_VECTOR_SIZE values even when
 * `count` is smaller.
 */

#ifndef SX_ENCODING_ENCODING_H
#define SX_ENCODING_ENCODING_H

typedef enum sx_encoding {
    SX_ENCODING_RAW = 0,
    SX_ENCODING_CONSTANT = 1,
    SX_ENCODING_RLE = 2,
    SX_ENCODING_FOR_BITPACK = 3,
    SX_ENCODING_DELTA_FOR = 4,
    SX_ENCODING_DELTA_CONST = 5,
    SX_ENCODING_ALP = 6,
    SX_ENCODING_ALP_RD = 7,
    SX_ENCODING_DICT = 8,
    SX_ENCODING_STRING = 9,
    SX_ENCODING_BITMAP = 10,
    SX_ENCODING_DELTA_PATCHED = 11,
} sx_encoding;

/* One more than the largest encoding id, for tables indexed by id. */
#define SX_ENCODING_ID_COUNT 12u

const char* sx_encoding_name(sx_encoding encoding);

#endif /* SX_ENCODING_ENCODING_H */
