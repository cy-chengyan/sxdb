/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sx_test.h"

#include "common/compiler.h"
#include "common/rng.h"
#include "encoding/bool_codec.h"
#include "encoding/string_codec.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------------------- */
/* BOOL                                                                                     */
/* ---------------------------------------------------------------------------------------- */

static bool bool_round_trip(const uint8_t* values, uint32_t count, sx_encoding* encoding) {
    uint8_t buffer[256];
    sx_byte_writer writer;
    sx_writer_init(&writer, buffer, sx_bool_encode_bound(count));
    if (sx_bool_encode(values, count, &writer, encoding) != SX_OK) {
        sx_test_fail(__FILE__, __LINE__, "bool encode failed");
        return false;
    }
    uint8_t decoded[SX_VECTOR_SIZE];
    if (sx_bool_decode(*encoding, buffer, writer.size, count, decoded) != SX_OK) {
        sx_test_fail(__FILE__, __LINE__, "bool decode failed");
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (decoded[i] != (values[i] != 0)) {
            sx_test_fail(__FILE__, __LINE__, "bool value %u differs", i);
            return false;
        }
    }
    return true;
}

static void test_bool_encodings(void) {
    uint8_t values[SX_VECTOR_SIZE] = {0};
    sx_encoding encoding = SX_ENCODING_RAW;
    SX_CHECK(bool_round_trip(values, SX_VECTOR_SIZE, &encoding));
    SX_CHECK(encoding == SX_ENCODING_CONSTANT);

    sx_rng rng;
    sx_rng_seed(&rng, 1);
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        values[i] = (uint8_t)(sx_rng_below(&rng, 10) == 0 ? 7 : 0);
    }
    SX_CHECK(bool_round_trip(values, SX_VECTOR_SIZE, &encoding));
    SX_CHECK(encoding == SX_ENCODING_BITMAP);
    SX_CHECK(bool_round_trip(values, 65, &encoding));
    SX_CHECK(bool_round_trip(values, 1, &encoding));
}

static void test_bool_corruption(void) {
    uint8_t payload[200] = {0};
    uint8_t decoded[SX_VECTOR_SIZE];
    SX_CHECK(sx_bool_decode(SX_ENCODING_BITMAP, payload, 127, SX_VECTOR_SIZE, decoded) ==
             SX_ERR_CORRUPT);
    SX_CHECK(sx_bool_decode(SX_ENCODING_BITMAP, payload, 128, SX_VECTOR_SIZE, decoded) == SX_OK);
    SX_CHECK(sx_bool_decode(SX_ENCODING_CONSTANT, payload, 9, 10, decoded) == SX_ERR_CORRUPT);
    SX_CHECK(sx_bool_decode(SX_ENCODING_ALP, payload, 8, 10, decoded) == SX_ERR_CORRUPT);
}

/* ---------------------------------------------------------------------------------------- */
/* VARCHAR                                                                                  */
/* ---------------------------------------------------------------------------------------- */

typedef struct string_column {
    uint32_t offsets[SX_VECTOR_SIZE + 1];
    uint8_t data[SX_VECTOR_SIZE * 64];
    uint32_t count;
} string_column;

static void column_reset(string_column* column) {
    column->offsets[0] = 0;
    column->count = 0;
}

static void column_append(string_column* column, const char* text) {
    size_t length = strlen(text);
    uint32_t start = column->offsets[column->count];
    memcpy(column->data + start, text, length);
    column->offsets[column->count + 1] = start + (uint32_t)length;
    column->count++;
}

static bool string_round_trip(const string_column* column, sx_encoding* encoding,
                              size_t* encoded_size) {
    static uint8_t buffer[SX_VECTOR_SIZE * 80];
    sx_byte_writer writer;
    sx_writer_init(&writer, buffer, sx_string_encode_bound(column->offsets, column->count));
    if (sx_string_encode(column->offsets, column->data, column->count, &writer, encoding) !=
        SX_OK) {
        sx_test_fail(__FILE__, __LINE__, "string encode failed");
        return false;
    }
    *encoded_size = writer.size;

    size_t data_bytes = 0;
    if (sx_string_decoded_size(*encoding, buffer, writer.size, column->count, &data_bytes) !=
            SX_OK ||
        data_bytes != column->offsets[column->count]) {
        sx_test_fail(__FILE__, __LINE__, "decoded size is wrong");
        return false;
    }

    static uint32_t offsets[SX_VECTOR_SIZE + 1];
    static uint8_t data[SX_VECTOR_SIZE * 64];
    if (sx_string_decode(*encoding, buffer, writer.size, column->count, offsets, data,
                         sizeof(data)) != SX_OK) {
        sx_test_fail(__FILE__, __LINE__, "string decode of %s failed", sx_encoding_name(*encoding));
        return false;
    }
    if (memcmp(offsets, column->offsets, (column->count + 1u) * sizeof(uint32_t)) != 0 ||
        memcmp(data, column->data, column->offsets[column->count]) != 0) {
        sx_test_fail(__FILE__, __LINE__, "decoded strings differ (%s)",
                     sx_encoding_name(*encoding));
        return false;
    }
    return true;
}

static void test_low_cardinality_strings_use_dict(void) {
    static string_column column;
    static const char* const sides[] = {"BUY", "SELL", "\xe4\xb9\xb0\xe5\x85\xa5"};
    column_reset(&column);
    sx_rng rng;
    sx_rng_seed(&rng, 2);
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        column_append(&column, sides[sx_rng_below(&rng, SX_ARRAY_SIZE(sides))]);
    }
    sx_encoding encoding = SX_ENCODING_RAW;
    size_t size = 0;
    SX_CHECK(string_round_trip(&column, &encoding, &size));
    SX_CHECK(encoding == SX_ENCODING_DICT);
    /* Header, packed dictionary lengths, dictionary bytes, and 2-bit codes. */
    SX_CHECK(size <= 16 + 128 + 16 + 256);
}

static void test_unique_strings_use_plain_encoding(void) {
    static string_column column;
    column_reset(&column);
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        char text[32];
        snprintf(text, sizeof(text), "order-%u-%u", i * 7919u, i);
        column_append(&column, text);
    }
    sx_encoding encoding = SX_ENCODING_RAW;
    size_t size = 0;
    SX_CHECK(string_round_trip(&column, &encoding, &size));
    SX_CHECK(encoding == SX_ENCODING_STRING);
}

static void test_constant_and_empty_strings(void) {
    static string_column column;
    column_reset(&column);
    for (uint32_t i = 0; i < 100; i++) {
        column_append(&column, "600519.SH");
    }
    sx_encoding encoding = SX_ENCODING_RAW;
    size_t size = 0;
    SX_CHECK(string_round_trip(&column, &encoding, &size));
    SX_CHECK(encoding == SX_ENCODING_CONSTANT);
    SX_CHECK_EQ_U64(size, 24);

    column_reset(&column);
    for (uint32_t i = 0; i < 50; i++) {
        column_append(&column, "");
    }
    SX_CHECK(string_round_trip(&column, &encoding, &size));
    SX_CHECK(encoding == SX_ENCODING_CONSTANT);

    column_reset(&column);
    column_append(&column, "");
    column_append(&column, "x");
    column_append(&column, "");
    SX_CHECK(string_round_trip(&column, &encoding, &size));
}

static void test_string_forced_encodings_and_limits(void) {
    static string_column column;
    column_reset(&column);
    for (uint32_t i = 0; i < 300; i++) {
        column_append(&column, (i % 3 == 0) ? "alpha" : "beta");
    }
    static uint8_t buffer[SX_VECTOR_SIZE * 80];
    static const sx_encoding encodings[] = {SX_ENCODING_STRING, SX_ENCODING_DICT};
    for (size_t e = 0; e < SX_ARRAY_SIZE(encodings); e++) {
        sx_byte_writer writer;
        sx_writer_init(&writer, buffer, sizeof(buffer));
        SX_CHECK(sx_string_encode_as(encodings[e], column.offsets, column.data, column.count,
                                     &writer) == SX_OK);
        uint32_t offsets[SX_VECTOR_SIZE + 1];
        uint8_t data[4096];
        SX_CHECK(sx_string_decode(encodings[e], buffer, writer.size, column.count, offsets, data,
                                  sizeof(data)) == SX_OK);
        SX_CHECK_EQ_MEM(data, column.data, column.offsets[column.count]);
        SX_CHECK(sx_string_decode(encodings[e], buffer, writer.size, column.count, offsets, data,
                                  10) == SX_ERR_LIMIT);
    }

    sx_byte_writer writer;
    sx_writer_init(&writer, buffer, sizeof(buffer));
    SX_CHECK(sx_string_encode_as(SX_ENCODING_CONSTANT, column.offsets, column.data, column.count,
                                 &writer) == SX_ERR_UNSUPPORTED);
}

static void test_random_string_payloads_never_crash(void) {
    static const sx_encoding encodings[] = {
        SX_ENCODING_CONSTANT,
        SX_ENCODING_STRING,
        SX_ENCODING_DICT,
    };
    sx_rng rng;
    sx_rng_seed(&rng, 3);
    static uint8_t payload[8192];
    static uint32_t offsets[SX_VECTOR_SIZE + 1];
    static uint8_t data[1 << 16];
    for (int iteration = 0; iteration < 20000; iteration++) {
        size_t size = sx_rng_below(&rng, sizeof(payload));
        for (size_t i = 0; i < size; i++) {
            payload[i] = (uint8_t)sx_rng_next(&rng);
        }
        if (size > 8 && sx_rng_below(&rng, 2) == 0) {
            /* Small plausible header fields reach deeper into the decoders. */
            payload[0] = (uint8_t)sx_rng_below(&rng, 64);
            payload[1] = 0;
            payload[2] = (uint8_t)sx_rng_below(&rng, 8);
            payload[3] = 0;
            payload[8] = (uint8_t)sx_rng_below(&rng, 33);
        }
        sx_encoding encoding = encodings[sx_rng_below(&rng, SX_ARRAY_SIZE(encodings))];
        uint32_t count = 1 + (uint32_t)sx_rng_below(&rng, SX_VECTOR_SIZE);
        sx_err err = sx_string_decode(encoding, payload, size, count, offsets, data, sizeof(data));
        SX_CHECK(err == SX_OK || err == SX_ERR_CORRUPT || err == SX_ERR_LIMIT);
    }
}

static const sx_test_case cases[] = {
    {"bool_encodings", test_bool_encodings},
    {"bool_corruption", test_bool_corruption},
    {"strings_low_cardinality_dict", test_low_cardinality_strings_use_dict},
    {"strings_unique_plain", test_unique_strings_use_plain_encoding},
    {"strings_constant_and_empty", test_constant_and_empty_strings},
    {"strings_forced_and_limits", test_string_forced_encodings_and_limits},
    {"strings_random_payloads", test_random_string_payloads_never_crash},
};

const sx_test_suite sx_test_suite_bool_string_codec = SX_TEST_SUITE("bool_string_codec", cases);
