/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sx_test.h"

#include "common/compiler.h"
#include "common/rng.h"
#include "encoding/int_codec.h"

#include <stdbool.h>
#include <stdlib.h>

#define NANOS_PER_MILLI INT64_C(1000000)
#define BASE_TIME_NS    (INT64_C(1792137600) * INT64_C(1000000000))

typedef struct encoded_chunk {
    uint8_t data[SX_VECTOR_SIZE * sizeof(int64_t)];
    size_t size;
    sx_encoding encoding;
} encoded_chunk;

/* Encodes, decodes, and compares. Reports details through sx_test_fail on mismatch. */
static bool round_trip(const int64_t* values, uint32_t count, encoded_chunk* chunk) {
    sx_byte_writer writer;
    sx_writer_init(&writer, chunk->data, sx_int64_encode_bound(count));
    if (sx_int64_encode(values, count, &writer, &chunk->encoding) != SX_OK) {
        sx_test_fail(__FILE__, __LINE__, "encode failed");
        return false;
    }
    chunk->size = writer.size;

    int64_t decoded[SX_VECTOR_SIZE];
    sx_err err = sx_int64_decode(chunk->encoding, chunk->data, chunk->size, count, decoded);
    if (err != SX_OK) {
        sx_test_fail(__FILE__, __LINE__, "decode of %s failed: %s",
                     sx_encoding_name(chunk->encoding), sx_err_str(err));
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (decoded[i] != values[i]) {
            sx_test_fail(__FILE__, __LINE__, "%s value %u: %" PRId64 " != %" PRId64,
                         sx_encoding_name(chunk->encoding), i, decoded[i], values[i]);
            return false;
        }
    }
    return true;
}

static void test_constant(void) {
    int64_t values[SX_VECTOR_SIZE];
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        values[i] = -42;
    }
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_CONSTANT);
    SX_CHECK_EQ_U64(chunk.size, 8);

    SX_CHECK(round_trip(values, 1, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_CONSTANT);
}

static void test_regular_timestamps_use_delta_const(void) {
    int64_t values[SX_VECTOR_SIZE];
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        values[i] = BASE_TIME_NS + (int64_t)i * INT64_C(1000000000);
    }
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_DELTA_CONST);
    SX_CHECK_EQ_U64(chunk.size, 16);

    SX_CHECK(round_trip(values, 2, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_DELTA_CONST);
}

static void test_irregular_millisecond_timestamps_use_delta_for(void) {
    int64_t values[SX_VECTOR_SIZE];
    sx_rng rng;
    sx_rng_seed(&rng, 1);
    int64_t now = BASE_TIME_NS;
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        now += (int64_t)(1 + sx_rng_below(&rng, 500)) * NANOS_PER_MILLI;
        values[i] = now;
    }
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_DELTA_FOR);
    /* Gaps up to 500 ms need 9 bits once the 10^6 factor is removed. */
    SX_CHECK(chunk.size <= 32 + 9 * 128);
}

static void test_scaled_prices_use_for_with_multiplier(void) {
    int64_t values[SX_VECTOR_SIZE];
    sx_rng rng;
    sx_rng_seed(&rng, 2);
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        /* DECIMAL(18,4) prices between 100.00 and 125.59 with two significant decimals. */
        values[i] = INT64_C(1000000) + (int64_t)sx_rng_below(&rng, 2560) * 100;
    }
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_FOR_BITPACK);
    SX_CHECK(chunk.size <= 24 + 12 * 128);
}

static void test_long_runs_use_rle(void) {
    int64_t values[SX_VECTOR_SIZE];
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        values[i] = INT64_C(5000000000) + (int64_t)(i / 200) * INT64_C(7777777);
    }
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_RLE);
}

static void test_random_values_fall_back_to_raw(void) {
    int64_t values[SX_VECTOR_SIZE];
    sx_rng rng;
    sx_rng_seed(&rng, 3);
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        values[i] = (int64_t)sx_rng_next(&rng);
    }
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_RAW);
}

static void test_extreme_values_round_trip(void) {
    static const int64_t extremes[] = {INT64_MIN, INT64_MAX, 0, -1, 1, INT64_MIN + 1};
    int64_t values[SX_VECTOR_SIZE];
    sx_rng rng;
    sx_rng_seed(&rng, 4);
    for (uint32_t count = 1; count <= SX_VECTOR_SIZE; count = count * 3 + 1) {
        for (uint32_t i = 0; i < count; i++) {
            values[i] = extremes[sx_rng_below(&rng, SX_ARRAY_SIZE(extremes))];
        }
        encoded_chunk chunk;
        SX_CHECK(round_trip(values, count, &chunk));
    }

    /* Differences that overflow int64 must still round-trip through DELTA encodings. */
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        values[i] = (i % 2 == 0) ? INT64_MIN : INT64_MAX;
    }
    uint8_t buffer[SX_VECTOR_SIZE * sizeof(int64_t)];
    sx_byte_writer writer;
    sx_writer_init(&writer, buffer, sizeof(buffer));
    SX_CHECK(sx_int64_encode_as(SX_ENCODING_DELTA_FOR, values, SX_VECTOR_SIZE, &writer) == SX_OK);
    int64_t decoded[SX_VECTOR_SIZE];
    SX_CHECK(sx_int64_decode(SX_ENCODING_DELTA_FOR, buffer, writer.size, SX_VECTOR_SIZE, decoded) ==
             SX_OK);
    SX_CHECK_EQ_MEM(decoded, values, sizeof(values));
}

static void test_forced_encodings_round_trip(void) {
    static const sx_encoding encodings[] = {
        SX_ENCODING_RAW,
        SX_ENCODING_RLE,
        SX_ENCODING_FOR_BITPACK,
        SX_ENCODING_DELTA_FOR,
    };
    int64_t values[SX_VECTOR_SIZE];
    sx_rng rng;
    sx_rng_seed(&rng, 5);
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        values[i] = (int64_t)sx_rng_below(&rng, 1000) - 500;
    }
    for (size_t e = 0; e < SX_ARRAY_SIZE(encodings); e++) {
        for (uint32_t count = 1; count <= SX_VECTOR_SIZE; count = count * 2 + 3) {
            if (encodings[e] == SX_ENCODING_DELTA_FOR && count < 2) {
                continue;
            }
            uint8_t buffer[SX_VECTOR_SIZE * 16];
            sx_byte_writer writer;
            sx_writer_init(&writer, buffer, sizeof(buffer));
            SX_CHECK(sx_int64_encode_as(encodings[e], values, count, &writer) == SX_OK);
            int64_t decoded[SX_VECTOR_SIZE];
            SX_CHECK(sx_int64_decode(encodings[e], buffer, writer.size, count, decoded) == SX_OK);
            SX_CHECK_EQ_MEM(decoded, values, count * sizeof(int64_t));
        }
    }

    uint8_t buffer[64];
    sx_byte_writer writer;
    sx_writer_init(&writer, buffer, sizeof(buffer));
    SX_CHECK(sx_int64_encode_as(SX_ENCODING_CONSTANT, values, 10, &writer) == SX_ERR_UNSUPPORTED);
    SX_CHECK(sx_int64_encode_as(SX_ENCODING_DELTA_CONST, values, 10, &writer) ==
             SX_ERR_UNSUPPORTED);
    SX_CHECK(sx_int64_encode_as(SX_ENCODING_RAW, values, 100, &writer) == SX_ERR_LIMIT);
    SX_CHECK_EQ_U64(writer.size, 0);
}

static void test_corrupted_payloads_are_rejected(void) {
    int64_t values[100];
    for (uint32_t i = 0; i < 100; i++) {
        values[i] = (int64_t)(i * 37 % 101);
    }
    uint8_t buffer[1024];
    sx_byte_writer writer;
    sx_writer_init(&writer, buffer, sizeof(buffer));
    SX_CHECK(sx_int64_encode_as(SX_ENCODING_FOR_BITPACK, values, 100, &writer) == SX_OK);
    int64_t decoded[SX_VECTOR_SIZE];

    SX_CHECK(sx_int64_decode(SX_ENCODING_FOR_BITPACK, buffer, writer.size - 1, 100, decoded) ==
             SX_ERR_CORRUPT);
    SX_CHECK(sx_int64_decode(SX_ENCODING_FOR_BITPACK, buffer, writer.size + 8, 100, decoded) ==
             SX_ERR_CORRUPT);
    buffer[16] = 65; /* width */
    SX_CHECK(sx_int64_decode(SX_ENCODING_FOR_BITPACK, buffer, writer.size, 100, decoded) ==
             SX_ERR_CORRUPT);
    SX_CHECK(sx_int64_decode((sx_encoding)99, buffer, writer.size, 100, decoded) == SX_ERR_CORRUPT);
    SX_CHECK(sx_int64_decode(SX_ENCODING_RAW, buffer, 0, 0, decoded) == SX_ERR_INVALID_ARG);

    /* RLE whose run lengths do not add up to the count. */
    sx_writer_init(&writer, buffer, sizeof(buffer));
    for (uint32_t i = 0; i < 100; i++) {
        values[i] = i / 10;
    }
    SX_CHECK(sx_int64_encode_as(SX_ENCODING_RLE, values, 100, &writer) == SX_OK);
    SX_CHECK(sx_int64_decode(SX_ENCODING_RLE, buffer, writer.size, 101, decoded) == SX_ERR_CORRUPT);
    SX_CHECK(sx_int64_decode(SX_ENCODING_RLE, buffer, writer.size, 99, decoded) == SX_ERR_CORRUPT);
}

#define NANOS_PER_MINUTE (INT64_C(60) * INT64_C(1000000000))

/* Appends the 241 minute bars of one trading day: 09:30 to 11:30 and 13:01 to 15:00. */
static uint32_t append_trading_day(int64_t* values, uint32_t count, int64_t day_start_ns) {
    for (int64_t minute = 9 * 60 + 30; minute <= 11 * 60 + 30; minute++) {
        values[count++] = day_start_ns + minute * NANOS_PER_MINUTE;
    }
    for (int64_t minute = 13 * 60 + 1; minute <= 15 * 60; minute++) {
        values[count++] = day_start_ns + minute * NANOS_PER_MINUTE;
    }
    return count;
}

static void test_trading_session_timestamps_use_delta_patched(void) {
    int64_t values[SX_VECTOR_SIZE + 256];
    uint32_t count = append_trading_day(values, 0, BASE_TIME_NS);
    SX_CHECK_EQ_U64(count, 241);
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, count, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_DELTA_PATCHED);
    /* Header, no packed bits for the regular steps, one exception for the lunch break. */
    SX_CHECK(chunk.size <= 32 + 8 + 8);

    /* Several days in one chunk: lunch breaks, nights, and a weekend are exceptions. */
    count = 0;
    static const int day_offsets[] = {0, 1, 2, 3, 6};
    for (size_t d = 0; d < SX_ARRAY_SIZE(day_offsets); d++) {
        count = append_trading_day(values, count,
                                   BASE_TIME_NS + day_offsets[d] * 1440 * 60 * INT64_C(1000000000));
    }
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_DELTA_PATCHED);
    SX_CHECK(chunk.size < 200);
}

static void test_patched_extreme_jumps_round_trip(void) {
    int64_t values[SX_VECTOR_SIZE];
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        if (i < 300) {
            values[i] = (int64_t)i;
        } else if (i < 700) {
            values[i] = INT64_MAX - 1000 + (int64_t)i;
        } else {
            values[i] = INT64_MIN + (int64_t)i;
        }
    }
    encoded_chunk chunk;
    SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
    SX_CHECK(chunk.encoding == SX_ENCODING_DELTA_PATCHED);

    uint8_t buffer[SX_VECTOR_SIZE * 16];
    sx_byte_writer writer;
    sx_writer_init(&writer, buffer, sizeof(buffer));
    SX_CHECK(sx_int64_encode_as(SX_ENCODING_DELTA_PATCHED, values, 2, &writer) ==
             SX_ERR_UNSUPPORTED);
}

static void test_common_multipliers_round_trip(void) {
    static const uint64_t multipliers[] = {
        1, 2, 3, 7, 100, 1000000, UINT64_C(60000000000), UINT64_C(1) << 40, UINT64_C(999999999989),
    };
    int64_t values[SX_VECTOR_SIZE];
    sx_rng rng;
    sx_rng_seed(&rng, 7);
    for (size_t m = 0; m < SX_ARRAY_SIZE(multipliers); m++) {
        for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
            values[i] = INT64_C(-5000000) + (int64_t)(sx_rng_below(&rng, 1000) * multipliers[m]);
        }
        encoded_chunk chunk;
        SX_CHECK(round_trip(values, SX_VECTOR_SIZE, &chunk));
        SX_CHECK(chunk.encoding == SX_ENCODING_FOR_BITPACK);
        /* Offsets below 1000 need 10 bits once the multiplier is divided out. */
        SX_CHECK(chunk.size <= 24 + 10 * 128);
    }
}

static void test_patched_corruption_is_rejected(void) {
    int64_t values[256];
    uint32_t count = append_trading_day(values, 0, BASE_TIME_NS);
    uint8_t buffer[1024];
    sx_byte_writer writer;
    sx_writer_init(&writer, buffer, sizeof(buffer));
    SX_CHECK(sx_int64_encode_as(SX_ENCODING_DELTA_PATCHED, values, count, &writer) == SX_OK);
    int64_t decoded[SX_VECTOR_SIZE];
    SX_CHECK(sx_int64_decode(SX_ENCODING_DELTA_PATCHED, buffer, writer.size, count, decoded) ==
             SX_OK);
    SX_CHECK(sx_int64_decode(SX_ENCODING_DELTA_PATCHED, buffer, writer.size - 1, count, decoded) ==
             SX_ERR_CORRUPT);

    /* The packed width is 0, so the exception list starts right after the 32-byte header. */
    uint8_t saved = buffer[33];
    buffer[33] = 0xFF; /* exception position far beyond the deltas */
    SX_CHECK(sx_int64_decode(SX_ENCODING_DELTA_PATCHED, buffer, writer.size, count, decoded) ==
             SX_ERR_CORRUPT);
    buffer[33] = saved;
    buffer[27] = 0x7F; /* exception count above the number of deltas */
    SX_CHECK(sx_int64_decode(SX_ENCODING_DELTA_PATCHED, buffer, writer.size, count, decoded) ==
             SX_ERR_CORRUPT);
}

static void test_random_payloads_never_crash(void) {
    sx_rng rng;
    sx_rng_seed(&rng, 6);
    uint8_t payload[4096];
    int64_t decoded[SX_VECTOR_SIZE];
    for (int iteration = 0; iteration < 20000; iteration++) {
        size_t size = sx_rng_below(&rng, sizeof(payload));
        for (size_t i = 0; i < size; i++) {
            payload[i] = (uint8_t)sx_rng_next(&rng);
        }
        sx_encoding encoding = (sx_encoding)sx_rng_below(&rng, SX_ENCODING_ID_COUNT);
        uint32_t count = 1 + (uint32_t)sx_rng_below(&rng, SX_VECTOR_SIZE);
        sx_err err = sx_int64_decode(encoding, payload, size, count, decoded);
        SX_CHECK(err == SX_OK || err == SX_ERR_CORRUPT);
    }
}

/*
 * The delta encodings store their deltas lane by lane (see bitpack.h), so every count
 * gives a different split of the slots over the lanes and a different last row.
 */
static void test_delta_encodings_every_count(void) {
    static const sx_encoding encodings[] = {SX_ENCODING_DELTA_FOR, SX_ENCODING_DELTA_PATCHED};
    int64_t values[SX_VECTOR_SIZE];
    sx_rng rng;
    sx_rng_seed(&rng, 11);
    int64_t value = BASE_TIME_NS;
    for (uint32_t i = 0; i < SX_VECTOR_SIZE; i++) {
        value += (int64_t)(1 + sx_rng_below(&rng, 500)) * NANOS_PER_MILLI;
        if (sx_rng_below(&rng, 100) == 0) {
            value += INT64_C(3600) * 1000 * NANOS_PER_MILLI; /* a gap for DELTA_PATCHED */
        }
        values[i] = value;
    }
    for (size_t e = 0; e < SX_ARRAY_SIZE(encodings); e++) {
        for (uint32_t count = 2; count <= SX_VECTOR_SIZE; count++) {
            uint8_t buffer[SX_VECTOR_SIZE * 16];
            sx_byte_writer writer;
            sx_writer_init(&writer, buffer, sizeof(buffer));
            sx_err err = sx_int64_encode_as(encodings[e], values, count, &writer);
            if (err == SX_ERR_UNSUPPORTED) {
                continue; /* DELTA_PATCHED applies only if it beats DELTA_FOR */
            }
            SX_CHECK(err == SX_OK);
            int64_t decoded[SX_VECTOR_SIZE];
            SX_CHECK(sx_int64_decode(encodings[e], buffer, writer.size, count, decoded) == SX_OK);
            SX_CHECK_EQ_MEM(decoded, values, count * sizeof(int64_t));
        }
    }
}

static const sx_test_case cases[] = {
    {"constant", test_constant},
    {"regular_timestamps", test_regular_timestamps_use_delta_const},
    {"irregular_ms_timestamps", test_irregular_millisecond_timestamps_use_delta_for},
    {"scaled_prices", test_scaled_prices_use_for_with_multiplier},
    {"long_runs", test_long_runs_use_rle},
    {"random_values_raw", test_random_values_fall_back_to_raw},
    {"extreme_values", test_extreme_values_round_trip},
    {"forced_encodings", test_forced_encodings_round_trip},
    {"corrupted_payloads", test_corrupted_payloads_are_rejected},
    {"trading_session_timestamps", test_trading_session_timestamps_use_delta_patched},
    {"patched_extreme_jumps", test_patched_extreme_jumps_round_trip},
    {"common_multipliers", test_common_multipliers_round_trip},
    {"patched_corruption", test_patched_corruption_is_rejected},
    {"random_payloads", test_random_payloads_never_crash},
    {"delta_encodings_every_count", test_delta_encodings_every_count},
};

const sx_test_suite sx_test_suite_int_codec = SX_TEST_SUITE("int_codec", cases);
