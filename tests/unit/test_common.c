/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sx_test.h"

#include "common/bits.h"
#include "common/bytes.h"
#include "common/clock.h"
#include "common/crc32c.h"
#include "common/error.h"
#include "common/hash.h"
#include "common/parse.h"
#include "common/rng.h"
#include "common/varint.h"
#include "os/os.h"

#include <stdlib.h>

static void test_crc32c_known_vectors(void) {
    SX_CHECK_EQ_U64(sx_crc32c(0, "", 0), 0x00000000u);
    SX_CHECK_EQ_U64(sx_crc32c(0, "123456789", 9), 0xE3069283u);
    SX_CHECK_EQ_U64(sx_crc32c_software(0, "123456789", 9), 0xE3069283u);

    uint8_t zeros[32] = {0};
    SX_CHECK_EQ_U64(sx_crc32c(0, zeros, sizeof(zeros)), 0x8A9136AAu);
}

static void test_crc32c_incremental_matches_one_shot(void) {
    const char* text = "The quick brown fox jumps over the lazy dog";
    size_t size = strlen(text);
    uint32_t one_shot = sx_crc32c(0, text, size);
    for (size_t split = 0; split <= size; split++) {
        uint32_t part = sx_crc32c(0, text, split);
        part = sx_crc32c(part, text + split, size - split);
        SX_CHECK_EQ_U64(part, one_shot);
    }
}

static void test_crc32c_hardware_matches_software(void) {
    uint8_t buffer[1000];
    sx_rng rng;
    sx_rng_seed(&rng, 42);
    for (size_t i = 0; i < sizeof(buffer); i++) {
        buffer[i] = (uint8_t)sx_rng_next(&rng);
    }
    for (size_t offset = 0; offset < 8; offset++) {
        for (size_t size = 0; size + offset <= sizeof(buffer); size += 37) {
            uint32_t fast = sx_crc32c(0, buffer + offset, size);
            uint32_t slow = sx_crc32c_software(0, buffer + offset, size);
            SX_CHECK_EQ_U64(fast, slow);
        }
    }
}

static void test_varint_round_trip(void) {
    const uint64_t values[] = {
        0, 1, 127, 128, 255, 300, 16383, 16384, UINT32_MAX, (uint64_t)1 << 56, UINT64_MAX,
    };
    for (size_t i = 0; i < SX_ARRAY_SIZE(values); i++) {
        uint8_t buffer[SX_VARINT_MAX_BYTES];
        size_t written = sx_varint_encode(values[i], buffer);
        uint64_t decoded = 0;
        size_t read = sx_varint_decode(buffer, written, &decoded);
        SX_CHECK_EQ_U64(read, written);
        SX_CHECK_EQ_U64(decoded, values[i]);
    }
}

static void test_varint_rejects_malformed_input(void) {
    uint64_t value = 0;
    const uint8_t truncated[] = {0x80, 0x80};
    SX_CHECK_EQ_U64(sx_varint_decode(truncated, sizeof(truncated), &value), 0);

    const uint8_t too_long[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02};
    SX_CHECK_EQ_U64(sx_varint_decode(too_long, sizeof(too_long), &value), 0);
}

static void test_bits_helpers(void) {
    SX_CHECK_EQ_U64(sx_bit_width_u64(0), 0);
    SX_CHECK_EQ_U64(sx_bit_width_u64(1), 1);
    SX_CHECK_EQ_U64(sx_bit_width_u64(255), 8);
    SX_CHECK_EQ_U64(sx_bit_width_u64(UINT64_MAX), 64);
    SX_CHECK_EQ_U64(sx_low_mask_u64(0), 0);
    SX_CHECK_EQ_U64(sx_low_mask_u64(3), 7);
    SX_CHECK_EQ_U64(sx_low_mask_u64(64), UINT64_MAX);

    SX_CHECK_EQ_U64(sx_gcd_u64(0, 0), 0);
    SX_CHECK_EQ_U64(sx_gcd_u64(0, 12), 12);
    SX_CHECK_EQ_U64(sx_gcd_u64(12, 18), 6);
    SX_CHECK_EQ_U64(sx_gcd_u64(1000000, 3000000), 1000000);
    SX_CHECK_EQ_U64(sx_gcd_u64(17, 5), 1);

    const int64_t signed_values[] = {0, -1, 1, -2, INT64_MIN, INT64_MAX};
    for (size_t i = 0; i < SX_ARRAY_SIZE(signed_values); i++) {
        SX_CHECK_EQ_I64(sx_zigzag_decode(sx_zigzag_encode(signed_values[i])), signed_values[i]);
    }
    SX_CHECK_EQ_U64(sx_zigzag_encode(-1), 1);
    SX_CHECK_EQ_U64(sx_zigzag_encode(1), 2);
}

/* Euclid's algorithm, the reference for the binary one. */
static uint64_t euclid_gcd(uint64_t a, uint64_t b) {
    while (b != 0) {
        uint64_t remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

static void test_gcd_matches_euclid(void) {
    static const uint64_t specials[] = {
        0, 1, 2, 3, 64, 100, UINT64_C(60000000000), UINT64_C(1) << 63, UINT64_MAX, UINT64_MAX - 1,
    };
    for (size_t i = 0; i < SX_ARRAY_SIZE(specials); i++) {
        for (size_t j = 0; j < SX_ARRAY_SIZE(specials); j++) {
            SX_CHECK_EQ_U64(sx_gcd_u64(specials[i], specials[j]),
                            euclid_gcd(specials[i], specials[j]));
        }
    }
    sx_rng rng;
    sx_rng_seed(&rng, 3);
    for (uint32_t round = 0; round < 100000; round++) {
        /* A shared factor, times random cofactors of random sizes. */
        uint64_t factor = sx_rng_next(&rng) >> sx_rng_below(&rng, 64);
        uint64_t a = factor * (sx_rng_next(&rng) >> sx_rng_below(&rng, 64));
        uint64_t b = factor * (sx_rng_next(&rng) >> sx_rng_below(&rng, 64));
        SX_CHECK_EQ_U64(sx_gcd_u64(a, b), euclid_gcd(a, b));
    }
}

static void test_byte_writer_and_reader_bounds(void) {
    uint8_t buffer[12];
    sx_byte_writer writer;
    sx_writer_init(&writer, buffer, sizeof(buffer));
    sx_writer_put_u32(&writer, 0xDEADBEEFu);
    sx_writer_put_u64(&writer, UINT64_C(0x0102030405060708));
    SX_CHECK(!writer.overflow);
    sx_writer_put_u8(&writer, 1);
    SX_CHECK(writer.overflow);
    SX_CHECK_EQ_U64(writer.size, 12);

    sx_byte_reader reader;
    sx_reader_init(&reader, buffer, sizeof(buffer));
    SX_CHECK_EQ_U64(sx_reader_get_u32(&reader), 0xDEADBEEFu);
    SX_CHECK_EQ_U64(sx_reader_get_u64(&reader), UINT64_C(0x0102030405060708));
    SX_CHECK(!reader.failed);
    SX_CHECK_EQ_U64(sx_reader_get_u16(&reader), 0);
    SX_CHECK(reader.failed);
}

static void test_parse_size(void) {
    uint64_t bytes = 0;
    SX_CHECK(sx_parse_size("0", &bytes) == SX_OK);
    SX_CHECK_EQ_U64(bytes, 0);
    SX_CHECK(sx_parse_size("1024", &bytes) == SX_OK);
    SX_CHECK_EQ_U64(bytes, 1024);
    SX_CHECK(sx_parse_size("64K", &bytes) == SX_OK);
    SX_CHECK_EQ_U64(bytes, 64u << 10);
    SX_CHECK(sx_parse_size("512m", &bytes) == SX_OK);
    SX_CHECK_EQ_U64(bytes, (uint64_t)512 << 20);
    SX_CHECK(sx_parse_size("8G", &bytes) == SX_OK);
    SX_CHECK_EQ_U64(bytes, (uint64_t)8 << 30);
    SX_CHECK(sx_parse_size("1T", &bytes) == SX_OK);
    SX_CHECK_EQ_U64(bytes, (uint64_t)1 << 40);

    const char* invalid[] = {
        "", "G", "8 G", "8GB", "8.5G", "-1", "50%", "8X", "99999999999999999999", "16777216T",
    };
    for (size_t i = 0; i < SX_ARRAY_SIZE(invalid); i++) {
        SX_CHECK(sx_parse_size(invalid[i], &bytes) == SX_ERR_INVALID_ARG);
    }
}

static void test_hash_is_deterministic_and_seeded(void) {
    const char* text = "series-key";
    uint64_t first = sx_hash_bytes(text, strlen(text), 1);
    uint64_t second = sx_hash_bytes(text, strlen(text), 1);
    uint64_t other_seed = sx_hash_bytes(text, strlen(text), 2);
    uint64_t other_text = sx_hash_bytes("series-kez", 10, 1);
    SX_CHECK_EQ_U64(first, second);
    SX_CHECK(first != other_seed);
    SX_CHECK(first != other_text);
    SX_CHECK(sx_hash_u64(1) != sx_hash_u64(2));
}

static void test_utc_formatting(void) {
    char text[SX_TIMESTAMP_TEXT_SIZE];
    /* 2026-10-16T08:00:00.123Z */
    int64_t unix_ns = INT64_C(1792137600) * SX_NANOS_PER_SECOND + INT64_C(123456789);
    sx_format_utc_millis(unix_ns, text);
    SX_CHECK_EQ_STR(text, "2026-10-16T08:00:00.123Z");
    sx_format_utc_compact(unix_ns, text);
    SX_CHECK_EQ_STR(text, "20261016T080000Z");

    sx_format_utc_millis(-1, text);
    SX_CHECK_EQ_STR(text, "1969-12-31T23:59:59.999Z");
}

static void test_os_system_information(void) {
    SX_CHECK(sx_os_physical_memory() > 0);
    SX_CHECK(sx_os_cpu_count() >= 1);
    SX_CHECK(sx_os_page_size() >= 4096);

    size_t size = 1u << 20;
    uint8_t* memory = sx_os_map_anonymous(size);
    SX_CHECK(memory != NULL);
    memory[0] = 1;
    memory[size - 1] = 2;
    sx_os_discard_pages(memory, size);
    sx_os_unmap(memory, size);
}

static void test_error_strings(void) {
    SX_CHECK_EQ_STR(sx_err_str(SX_OK), "success");
    SX_CHECK_EQ_STR(sx_err_str(SX_ERR_CORRUPT), "data is corrupted");
}

static const sx_test_case cases[] = {
    {"crc32c_known_vectors", test_crc32c_known_vectors},
    {"crc32c_incremental", test_crc32c_incremental_matches_one_shot},
    {"crc32c_hardware_matches_software", test_crc32c_hardware_matches_software},
    {"varint_round_trip", test_varint_round_trip},
    {"varint_malformed", test_varint_rejects_malformed_input},
    {"bits_helpers", test_bits_helpers},
    {"gcd_matches_euclid", test_gcd_matches_euclid},
    {"byte_writer_reader_bounds", test_byte_writer_and_reader_bounds},
    {"parse_size", test_parse_size},
    {"hash", test_hash_is_deterministic_and_seeded},
    {"utc_formatting", test_utc_formatting},
    {"os_system_information", test_os_system_information},
    {"error_strings", test_error_strings},
};

const sx_test_suite sx_test_suite_common = SX_TEST_SUITE("common", cases);
