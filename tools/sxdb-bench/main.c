/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * sxdb-bench: measures the compression ratio and the encode and decode speed of the
 * column encodings, on built-in synthetic datasets or on one column of a CSV file.
 *
 * Usage:
 *   sxdb-bench [--values N] [--min-time SECONDS] [DATASET ...]
 *   sxdb-bench --csv FILE --column N --type int64|double|bool|string [--skip-header]
 *   sxdb-bench --list
 *
 * Every dataset is split into chunks of 1024 values, as the storage engine does. The
 * first decode pass is checked against the input, so a wrong codec cannot report a
 * result.
 */

#include "column.h"
#include "csv.h"
#include "datasets.h"

#include "common/bytes.h"
#include "common/clock.h"
#include "common/compiler.h"
#include "encoding/bool_codec.h"
#include "encoding/encoding.h"
#include "encoding/float_codec.h"
#include "encoding/int_codec.h"
#include "encoding/string_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENCODING_ID_COUNT     11u
#define DEFAULT_VALUE_COUNT   (1u << 20)
#define DEFAULT_MIN_SECONDS   0.25

typedef struct bench_options {
    uint32_t value_count;
    double min_seconds;
    const char* csv_path;
    unsigned csv_column;
    bench_type csv_type;
    bool csv_type_given;
    bool skip_header;
} bench_options;

/* Every chunk of a column, encoded back to back. */
typedef struct encoded_column {
    uint32_t chunk_count;
    uint8_t* data;
    size_t capacity;
    size_t* chunk_offsets; /* chunk_count + 1 entries */
    sx_encoding* encodings;
} encoded_column;

typedef struct bench_result {
    uint64_t raw_bytes;
    uint64_t encoded_bytes;
    double encode_seconds;
    double decode_seconds;
    uint32_t encoding_counts[ENCODING_ID_COUNT];
} bench_result;

/* Decode output buffers for one chunk. */
typedef struct decode_buffers {
    int64_t ints[SX_VECTOR_SIZE];
    double doubles[SX_VECTOR_SIZE];
    uint8_t bools[SX_VECTOR_SIZE];
    uint32_t offsets[SX_VECTOR_SIZE + 1];
    uint8_t* string_data;
    size_t string_capacity;
} decode_buffers;

/* ---------------------------------------------------------------------------------------- */
/* Chunking                                                                                 */
/* ---------------------------------------------------------------------------------------- */

static uint32_t chunk_start(uint32_t chunk) {
    return chunk * SX_VECTOR_SIZE;
}

static uint32_t chunk_size(const bench_column* column, uint32_t chunk) {
    uint32_t start = chunk_start(chunk);
    uint32_t remaining = column->count - start;
    return remaining < SX_VECTOR_SIZE ? remaining : SX_VECTOR_SIZE;
}

static size_t chunk_bound(const bench_column* column, uint32_t chunk) {
    uint32_t start = chunk_start(chunk);
    uint32_t count = chunk_size(column, chunk);
    switch (column->type) {
        case BENCH_TYPE_INT64:
            return sx_int64_encode_bound(count);
        case BENCH_TYPE_DOUBLE:
            return sx_double_encode_bound(count);
        case BENCH_TYPE_BOOL:
            return sx_bool_encode_bound(count);
        case BENCH_TYPE_STRING:
            return sx_string_encode_bound(column->offsets + start, count);
    }
    return 0;
}

static bool encoded_column_init(encoded_column* encoded, const bench_column* column) {
    memset(encoded, 0, sizeof(*encoded));
    encoded->chunk_count = (column->count + SX_VECTOR_SIZE - 1u) / SX_VECTOR_SIZE;
    for (uint32_t chunk = 0; chunk < encoded->chunk_count; chunk++) {
        encoded->capacity += chunk_bound(column, chunk);
    }
    encoded->data = malloc(encoded->capacity);
    encoded->chunk_offsets = calloc((size_t)encoded->chunk_count + 1u, sizeof(size_t));
    encoded->encodings = calloc(encoded->chunk_count, sizeof(sx_encoding));
    return encoded->data != NULL && encoded->chunk_offsets != NULL && encoded->encodings != NULL;
}

static void encoded_column_free(encoded_column* encoded) {
    free(encoded->data);
    free(encoded->chunk_offsets);
    free(encoded->encodings);
}

/* ---------------------------------------------------------------------------------------- */
/* Encode and decode passes                                                                 */
/* ---------------------------------------------------------------------------------------- */

static sx_err encode_chunk(const bench_column* column, uint32_t chunk, sx_byte_writer* writer,
                           sx_encoding* encoding) {
    uint32_t start = chunk_start(chunk);
    uint32_t count = chunk_size(column, chunk);
    switch (column->type) {
        case BENCH_TYPE_INT64:
            return sx_int64_encode(column->ints + start, count, writer, encoding);
        case BENCH_TYPE_DOUBLE:
            return sx_double_encode(column->doubles + start, count, writer, encoding);
        case BENCH_TYPE_BOOL:
            return sx_bool_encode(column->bools + start, count, writer, encoding);
        case BENCH_TYPE_STRING:
            return sx_string_encode(column->offsets + start, column->string_data, count, writer,
                                    encoding);
    }
    return SX_ERR_UNSUPPORTED;
}

static bool encode_pass(const bench_column* column, encoded_column* encoded) {
    size_t offset = 0;
    for (uint32_t chunk = 0; chunk < encoded->chunk_count; chunk++) {
        sx_byte_writer writer;
        sx_writer_init(&writer, encoded->data + offset, chunk_bound(column, chunk));
        if (encode_chunk(column, chunk, &writer, &encoded->encodings[chunk]) != SX_OK) {
            fprintf(stderr, "error: encoding chunk %u failed\n", chunk);
            return false;
        }
        encoded->chunk_offsets[chunk] = offset;
        offset += writer.size;
    }
    encoded->chunk_offsets[encoded->chunk_count] = offset;
    return true;
}

/* Compares a decoded chunk with the input. */
static bool chunk_matches(const bench_column* column, uint32_t chunk,
                          const decode_buffers* buffers) {
    uint32_t start = chunk_start(chunk);
    uint32_t count = chunk_size(column, chunk);
    switch (column->type) {
        case BENCH_TYPE_INT64:
            return memcmp(buffers->ints, column->ints + start, count * sizeof(int64_t)) == 0;
        case BENCH_TYPE_DOUBLE:
            return memcmp(buffers->doubles, column->doubles + start, count * sizeof(double)) == 0;
        case BENCH_TYPE_BOOL:
            for (uint32_t i = 0; i < count; i++) {
                if (buffers->bools[i] != (column->bools[start + i] != 0)) {
                    return false;
                }
            }
            return true;
        case BENCH_TYPE_STRING: {
            uint32_t base = column->offsets[start];
            for (uint32_t i = 0; i <= count; i++) {
                if (buffers->offsets[i] != column->offsets[start + i] - base) {
                    return false;
                }
            }
            return memcmp(buffers->string_data, column->string_data + base,
                          buffers->offsets[count]) == 0;
        }
    }
    return false;
}

static sx_err decode_chunk(const bench_column* column, const encoded_column* encoded,
                           uint32_t chunk, decode_buffers* buffers) {
    const uint8_t* payload = encoded->data + encoded->chunk_offsets[chunk];
    size_t size = encoded->chunk_offsets[chunk + 1] - encoded->chunk_offsets[chunk];
    sx_encoding encoding = encoded->encodings[chunk];
    uint32_t count = chunk_size(column, chunk);
    switch (column->type) {
        case BENCH_TYPE_INT64:
            return sx_int64_decode(encoding, payload, size, count, buffers->ints);
        case BENCH_TYPE_DOUBLE:
            return sx_double_decode(encoding, payload, size, count, buffers->doubles);
        case BENCH_TYPE_BOOL:
            return sx_bool_decode(encoding, payload, size, count, buffers->bools);
        case BENCH_TYPE_STRING:
            return sx_string_decode(encoding, payload, size, count, buffers->offsets,
                                    buffers->string_data, buffers->string_capacity);
    }
    return SX_ERR_UNSUPPORTED;
}

static bool decode_pass(const bench_column* column, const encoded_column* encoded,
                        decode_buffers* buffers, bool verify) {
    for (uint32_t chunk = 0; chunk < encoded->chunk_count; chunk++) {
        sx_err err = decode_chunk(column, encoded, chunk, buffers);
        if (err != SX_OK) {
            fprintf(stderr, "error: decoding chunk %u failed: %s\n", chunk, sx_err_str(err));
            return false;
        }
        if (verify && !chunk_matches(column, chunk, buffers)) {
            fprintf(stderr, "error: chunk %u does not round-trip\n", chunk);
            return false;
        }
    }
    return true;
}

static double seconds_since(uint64_t start_ns) {
    return (double)(sx_clock_monotonic_ns() - start_ns) / 1e9;
}

/* Runs one pass to warm up, then repeats until `min_seconds` have passed. */
static bool time_encoding(const bench_column* column, encoded_column* encoded,
                          double min_seconds, double* seconds_per_pass) {
    if (!encode_pass(column, encoded)) {
        return false;
    }
    uint32_t passes = 0;
    uint64_t start = sx_clock_monotonic_ns();
    do {
        if (!encode_pass(column, encoded)) {
            return false;
        }
        passes++;
    } while (seconds_since(start) < min_seconds);
    *seconds_per_pass = seconds_since(start) / passes;
    return true;
}

static bool time_decoding(const bench_column* column, const encoded_column* encoded,
                          decode_buffers* buffers, double min_seconds, double* seconds_per_pass) {
    if (!decode_pass(column, encoded, buffers, true)) {
        return false;
    }
    uint32_t passes = 0;
    uint64_t start = sx_clock_monotonic_ns();
    do {
        if (!decode_pass(column, encoded, buffers, false)) {
            return false;
        }
        passes++;
    } while (seconds_since(start) < min_seconds);
    *seconds_per_pass = seconds_since(start) / passes;
    return true;
}

static bool run_benchmark(const bench_column* column, double min_seconds, bench_result* result) {
    memset(result, 0, sizeof(*result));
    encoded_column encoded;
    decode_buffers* buffers = calloc(1, sizeof(*buffers));
    bool ok = buffers != NULL && encoded_column_init(&encoded, column);
    if (ok && column->type == BENCH_TYPE_STRING) {
        /* Enough for the largest chunk of string data. */
        buffers->string_capacity = column->offsets[column->count] + 1u;
        buffers->string_data = malloc(buffers->string_capacity);
        ok = buffers->string_data != NULL;
    }

    ok = ok && time_encoding(column, &encoded, min_seconds, &result->encode_seconds);
    ok = ok && time_decoding(column, &encoded, buffers, min_seconds, &result->decode_seconds);
    if (ok) {
        result->raw_bytes = bench_column_raw_bytes(column);
        result->encoded_bytes = encoded.chunk_offsets[encoded.chunk_count];
        for (uint32_t chunk = 0; chunk < encoded.chunk_count; chunk++) {
            result->encoding_counts[encoded.encodings[chunk]]++;
        }
    }

    encoded_column_free(&encoded);
    if (buffers != NULL) {
        free(buffers->string_data);
        free(buffers);
    }
    return ok;
}

/* ---------------------------------------------------------------------------------------- */
/* Reporting                                                                                */
/* ---------------------------------------------------------------------------------------- */

static void print_header(void) {
    printf("%-18s %-7s %9s %9s %8s %10s %10s %10s  %s\n", "dataset", "type", "values",
           "bytes/val", "ratio", "enc MB/s", "dec MB/s", "dec Mval/s", "encodings");
    printf("%-18s %-7s %9s %9s %8s %10s %10s %10s  %s\n", "-------", "----", "------",
           "---------", "-----", "--------", "--------", "----------", "---------");
}

/* Lists the encodings used, most frequent first, with their share of chunks. */
static void format_encodings(const bench_result* result, uint32_t chunks, char* out, size_t size) {
    bool used[ENCODING_ID_COUNT] = {false};
    size_t written = 0;
    out[0] = '\0';
    for (;;) {
        int best = -1;
        for (unsigned id = 0; id < ENCODING_ID_COUNT; id++) {
            if (!used[id] && result->encoding_counts[id] > 0 &&
                (best < 0 || result->encoding_counts[id] > result->encoding_counts[best])) {
                best = (int)id;
            }
        }
        if (best < 0 || written >= size) {
            return;
        }
        used[best] = true;
        double share = 100.0 * result->encoding_counts[best] / chunks;
        int n = snprintf(out + written, size - written, "%s%s %.0f%%", written > 0 ? ", " : "",
                         sx_encoding_name((sx_encoding)best), share);
        if (n < 0) {
            return;
        }
        written += (size_t)n;
    }
}

static void print_result(const char* name, const bench_column* column,
                         const bench_result* result) {
    uint32_t chunks = (column->count + SX_VECTOR_SIZE - 1u) / SX_VECTOR_SIZE;
    double bytes_per_value = (double)result->encoded_bytes / column->count;
    double ratio = (double)result->raw_bytes / (double)result->encoded_bytes;
    double encode_mb = (double)result->raw_bytes / result->encode_seconds / 1e6;
    double decode_mb = (double)result->raw_bytes / result->decode_seconds / 1e6;
    double decode_mvalues = (double)column->count / result->decode_seconds / 1e6;
    char encodings[128];
    format_encodings(result, chunks, encodings, sizeof(encodings));
    printf("%-18s %-7s %9u %9.3f %7.1fx %10.0f %10.0f %10.0f  %s\n", name,
           bench_type_name(column->type), column->count, bytes_per_value, ratio, encode_mb,
           decode_mb, decode_mvalues, encodings);
}

/* ---------------------------------------------------------------------------------------- */
/* Command line                                                                             */
/* ---------------------------------------------------------------------------------------- */

static void print_usage(void) {
    printf("usage: sxdb-bench [--values N] [--min-time SECONDS] [DATASET ...]\n"
           "       sxdb-bench --csv FILE --column N --type int64|double|bool|string"
           " [--skip-header]\n"
           "       sxdb-bench --list\n");
}

static void print_datasets(void) {
    for (size_t i = 0; i < bench_dataset_count; i++) {
        printf("%-18s %-7s %s\n", bench_datasets[i].name, bench_type_name(bench_datasets[i].type),
               bench_datasets[i].description);
    }
}

static bool parse_type(const char* text, bench_type* type) {
    static const bench_type types[] = {
        BENCH_TYPE_INT64,
        BENCH_TYPE_DOUBLE,
        BENCH_TYPE_BOOL,
        BENCH_TYPE_STRING,
    };
    for (size_t i = 0; i < SX_ARRAY_SIZE(types); i++) {
        if (strcmp(text, bench_type_name(types[i])) == 0) {
            *type = types[i];
            return true;
        }
    }
    return false;
}

static const bench_dataset* find_dataset(const char* name) {
    for (size_t i = 0; i < bench_dataset_count; i++) {
        if (strcmp(bench_datasets[i].name, name) == 0) {
            return &bench_datasets[i];
        }
    }
    return NULL;
}

static bool run_dataset(const bench_dataset* dataset, const bench_options* options) {
    bench_column column;
    bool ok = bench_column_init(&column, dataset->type, options->value_count, 1u << 20) &&
              dataset->generate(&column, 42);
    bench_result result;
    ok = ok && run_benchmark(&column, options->min_seconds, &result);
    if (ok) {
        print_result(dataset->name, &column, &result);
    } else {
        fprintf(stderr, "error: dataset %s failed\n", dataset->name);
    }
    bench_column_free(&column);
    return ok;
}

static int run_csv(const bench_options* options) {
    bench_column column;
    char error[512];
    if (!bench_load_csv(options->csv_path, options->csv_column, options->csv_type,
                        options->skip_header, &column, error, sizeof(error))) {
        fprintf(stderr, "error: %s\n", error);
        return 1;
    }
    bench_result result;
    bool ok = run_benchmark(&column, options->min_seconds, &result);
    if (ok) {
        print_header();
        print_result("csv", &column, &result);
    }
    bench_column_free(&column);
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    bench_options options = {
        .value_count = DEFAULT_VALUE_COUNT,
        .min_seconds = DEFAULT_MIN_SECONDS,
    };
    const char* selected[64];
    size_t selected_count = 0;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        bool has_value = i + 1 < argc;
        if (strcmp(arg, "--list") == 0) {
            print_datasets();
            return 0;
        } else if (strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(arg, "--values") == 0 && has_value) {
            long value = strtol(argv[++i], NULL, 10);
            if (value < 1 || value > 100000000) {
                fprintf(stderr, "error: --values must be between 1 and 100000000\n");
                return 2;
            }
            options.value_count = (uint32_t)value;
        } else if (strcmp(arg, "--min-time") == 0 && has_value) {
            options.min_seconds = strtod(argv[++i], NULL);
        } else if (strcmp(arg, "--csv") == 0 && has_value) {
            options.csv_path = argv[++i];
        } else if (strcmp(arg, "--column") == 0 && has_value) {
            options.csv_column = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(arg, "--type") == 0 && has_value) {
            if (!parse_type(argv[++i], &options.csv_type)) {
                fprintf(stderr, "error: unknown type %s\n", argv[i]);
                return 2;
            }
            options.csv_type_given = true;
        } else if (strcmp(arg, "--skip-header") == 0) {
            options.skip_header = true;
        } else if (arg[0] != '-' && selected_count < SX_ARRAY_SIZE(selected)) {
            selected[selected_count] = arg;
            selected_count++;
        } else {
            print_usage();
            return 2;
        }
    }

    if (options.csv_path != NULL) {
        if (!options.csv_type_given) {
            fprintf(stderr, "error: --csv needs --type\n");
            return 2;
        }
        return run_csv(&options);
    }

    print_header();
    bool ok = true;
    if (selected_count == 0) {
        for (size_t i = 0; i < bench_dataset_count; i++) {
            ok = run_dataset(&bench_datasets[i], &options) && ok;
        }
    } else {
        for (size_t i = 0; i < selected_count; i++) {
            const bench_dataset* dataset = find_dataset(selected[i]);
            if (dataset == NULL) {
                fprintf(stderr, "error: unknown dataset %s (see --list)\n", selected[i]);
                return 2;
            }
            ok = run_dataset(dataset, &options) && ok;
        }
    }
    return ok ? 0 : 1;
}
