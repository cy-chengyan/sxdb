/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sx_test.h"

#include "common/bits.h"
#include "common/compiler.h"
#include "common/rng.h"
#include "encoding/float_codec.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>

typedef struct encoded_chunk {
    uint8_t data[SX_VECTOR_SIZE * sizeof(double)];
    size_t size;
    sx_encoding encoding;
} encoded_chunk;

/* Encodes, decodes, and compares bit for bit. */
static bool round_trip(const double* values, uint32_t count, encoded_chunk* chunk) {
    sx_byte_writer writer;
    sx_writer_init(&writer, chunk->data, sx_double_encode_bound(count));
    if (sx_double_encode(values, count, &writer, &chunk->encoding) != SX_OK) {
        sx_test_fail(__FILE__, __LINE__, "encode failed");
        return false;
    }
    chunk->size = writer.size;

    double decoded[SX_VECTOR_SIZE];
    sx_err err = sx_double_decode(chunk->encoding, chunk->data, chunk->size, count, decoded);
    if (err != SX_OK) {
        sx_test_fail(__FILE__, __LINE__, "decode of %s failed: %s",
                     sx_encoding_name(chunk->encoding), sx_err_str(err));
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (sx_double_to_bits(decoded[i]) != sx_double_to_bits(values[i])) {
            sx_test_fail(__FILE__, __LINE__, "%s value %u: %.17g != %.17g",
                         sx_encoding_name(chunk->encoding), i, decoded[i], values[i]);
            return false;
        }
    }
    return true;
}

static void fill_price_walk(double* values, uint32_t count, uint64_t seed) {
    sx_rng rng;
    sx_rng_seed(&rng, seed);
    int64_t cents = 1856000; /* 18560.00 */
    for (uint32_t i = 0; i < count; i++) {
        cents += (int64_t)sx_rng_below(&rng, 21) - 10;
        values[i] = (double)cents / 100.0;
    }
}

static void test_decimal_prices_use_alp(void) {
    double values[SX_VECTOR_SIZE];
    fill_price_walk(values, SX_VECTOR_SIZE, 1);
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_ALP);
    SX_CHECK(chunk.size < SX_VECTOR_SIZE * 3);
}

static void test_one_decimal_percentages_use_alp(void) {
    double values[SX_VECTOR_SIZE];
    sx_rng rng;
    sx_rng_seed(&rng, 2);
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        values[i] = (double)sx_rng_below(&rng, 1001) / 10.0;
    }
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_ALP);
    SX_CHECK(chunk.size <= 24 + 10 * 128);
}

static void test_special_values_round_trip(void) {
    const double specials[] = {
        NAN,      -NAN,
        INFINITY, -INFINITY,
        -0.0,     0.0,
        DBL_MIN,  DBL_MAX,
        -DBL_MAX, 4.9406564584124654e-324,
        1e300,    -1e-300,
        0.1,      123456789012345678.0,
    };
    double values[SX_VECTOR_SIZE];
    fill_price_walk(values, SX_VECTOR_SIZE, 3);
    for (size_t i = 0; i < SX_ARRAY_SIZE(specials); i++) {
        values[i * 70] = specials[i];
    }
    /* A NaN with a custom payload must keep its payload. */
    values[1000] = sx_bits_to_double(UINT64_C(0x7FF8DEADBEEF0001));

    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_ALP);

    SX_CHECK(round_trip(specials, SX_ARRAY_SIZE(specials), &chunk));
}

static void test_full_precision_doubles_round_trip(void) {
    double values[SX_VECTOR_SIZE];
    sx_rng rng;
    sx_rng_seed(&rng, 4);
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        values[i] = 20.0 + sx_rng_next_double(&rng) * 10.0;
    }
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_ALP_RD);
    SX_CHECK(chunk.size < SX_VECTOR_SIZE * sizeof(double));
}

static void test_constant_and_signed_zero(void) {
    double values[SX_VECTOR_SIZE];
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        values[i] = NAN;
    }
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_CONSTANT);

    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        values[i] = (i % 2 == 0) ? 0.0 : -0.0;
    }
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding != SX_ENCODING_CONSTANT);

    values[0] = 42.5;
    SX_CHECK(round_trip(values, 1, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_CONSTANT);
}

static void test_widened_floats_round_trip(void) {
    double values[SX_VECTOR_SIZE];
    sx_rng rng;
    sx_rng_seed(&rng, 5);
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        float reading = 15.0f + (float)sx_rng_below(&rng, 3000) / 100.0f;
        values[i] = (double)reading;
    }
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    /* The 29 always-zero low bits of widened floats must not be stored. */
    SX_CHECK(chunk.size < SX_VECTOR_SIZE * 4);
    double decoded[SX_VECTOR_SIZE];
    SX_CHECK(sx_double_decode(chunk.encoding, chunk.data, chunk.size, SX_VECTOR_SIZE, decoded) ==
             SX_OK);
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        SX_CHECK((float)decoded[i] == (float)values[i]);
    }
}

static void test_forced_encodings_round_trip(void) {
    static const sx_encoding encodings[] = {SX_ENCODING_RAW, SX_ENCODING_ALP, SX_ENCODING_ALP_RD};
    double values[SX_VECTOR_SIZE];
    sx_rng rng;
    sx_rng_seed(&rng, 6);
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        values[i] = (sx_rng_next_double(&rng) - 0.5) * 1e6;
    }
    for (size_t e = 0; e < SX_ARRAY_SIZE(encodings); e++) {
        for (uint32_t count = 1; count <= SX_VECTOR_SIZE; count = count * 2 + 5) {
            uint8_t buffer[SX_VECTOR_SIZE * 24];
            sx_byte_writer writer;
            sx_writer_init(&writer, buffer, sizeof(buffer));
            SX_CHECK(sx_double_encode_as(encodings[e], values, count, &writer) == SX_OK);
            double decoded[SX_VECTOR_SIZE];
            SX_CHECK(sx_double_decode(encodings[e], buffer, writer.size, count, decoded) == SX_OK);
            SX_CHECK_EQ_MEM(decoded, values, count * sizeof(double));
        }
    }
}

static void test_corrupted_payloads_are_rejected(void) {
    double values[100];
    fill_price_walk(values, 100, 7);
    uint8_t buffer[2048];
    sx_byte_writer writer;
    sx_writer_init(&writer, buffer, sizeof(buffer));
    SX_CHECK(sx_double_encode_as(SX_ENCODING_ALP, values, 100, &writer) == SX_OK);
    double decoded[SX_VECTOR_SIZE];

    SX_CHECK(sx_double_decode(SX_ENCODING_ALP, buffer, writer.size - 1, 100, decoded) ==
             SX_ERR_CORRUPT);
    uint8_t saved = buffer[0];
    buffer[0] = 19; /* exponent above 18 */
    SX_CHECK(sx_double_decode(SX_ENCODING_ALP, buffer, writer.size, 100, decoded) ==
             SX_ERR_CORRUPT);
    buffer[0] = saved;
    SX_CHECK(sx_double_decode(SX_ENCODING_ALP, buffer, writer.size, 100, decoded) == SX_OK);
    SX_CHECK(sx_double_decode(SX_ENCODING_DELTA_FOR, buffer, writer.size, 100, decoded) ==
             SX_ERR_CORRUPT);
}

static void test_random_payloads_never_crash(void) {
    static const sx_encoding encodings[] = {
        SX_ENCODING_RAW,
        SX_ENCODING_CONSTANT,
        SX_ENCODING_ALP,
        SX_ENCODING_ALP_RD,
    };
    sx_rng rng;
    sx_rng_seed(&rng, 8);
    uint8_t payload[4096];
    double decoded[SX_VECTOR_SIZE];
    for (int iteration = 0; iteration < 20000; iteration++) {
        size_t size = sx_rng_below(&rng, sizeof(payload));
        for (size_t i = 0; i < size; i++) {
            payload[i] = (uint8_t)sx_rng_next(&rng);
        }
        /* Plausible headers reach deeper into the decoders than pure noise. */
        if (size > 4 && sx_rng_below(&rng, 2) == 0) {
            payload[0] = (uint8_t)sx_rng_below(&rng, 64);
            payload[1] = (uint8_t)sx_rng_below(&rng, 19);
            payload[2] = (uint8_t)sx_rng_below(&rng, 4);
        }
        sx_encoding encoding = encodings[sx_rng_below(&rng, SX_ARRAY_SIZE(encodings))];
        uint32_t count = 1 + (uint32_t)sx_rng_below(&rng, SX_VECTOR_SIZE);
        sx_err err = sx_double_decode(encoding, payload, size, count, decoded);
        SX_CHECK(err == SX_OK || err == SX_ERR_CORRUPT);
    }
}

static const sx_test_case cases[] = {
    {"decimal_prices_alp", test_decimal_prices_use_alp},
    {"percentages_alp", test_one_decimal_percentages_use_alp},
    {"special_values", test_special_values_round_trip},
    {"full_precision_alp_rd", test_full_precision_doubles_round_trip},
    {"constant_and_signed_zero", test_constant_and_signed_zero},
    {"widened_floats", test_widened_floats_round_trip},
    {"forced_encodings", test_forced_encodings_round_trip},
    {"corrupted_payloads", test_corrupted_payloads_are_rejected},
    {"random_payloads", test_random_payloads_never_crash},
};

const sx_test_suite sx_test_suite_float_codec = SX_TEST_SUITE("float_codec", cases);
