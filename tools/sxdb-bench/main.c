/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * sxdb-bench: measures the compression ratio and the encode and decode speed of the
 * column encodings, on built-in synthetic datasets or on columns of real CSV data.
 *
 * Usage:
 *   sxdb-bench [options] [DATASET ...]
 *   sxdb-bench [options] --csv PATH [PATH ...] --columns SPEC [--skip-header] [--split-on N]
 *   sxdb-bench --list
 *
 * Options:
 *   --values N        values per synthetic dataset (default 1048576)
 *   --min-time S      minimum seconds to time each measurement (default 0.25)
 *   --threads N       encode and decode with N threads (default 1)
 *   --simd LEVEL      codec kernels: scalar, avx2, avx512, or neon (default: the best the
 *                     CPU supports)
 *
 * Synthetic datasets are cut into chunks of 1024 values. CSV input is cut the way the
 * storage engine cuts blocks: at most 1024 values, never across two files (series), and
 * with --split-on never across a change of that field (a partition such as a day).
 * The first decode pass is checked against the input, so a wrong codec cannot report a
 * result.
 */

#include "chunks.h"
#include "column.h"
#include "csv.h"
#include "datasets.h"

#include "common/bits.h"
#include "common/bytes.h"
#include "common/clock.h"
#include "common/compiler.h"
#include "encoding/bool_codec.h"
#include "encoding/encoding.h"
#include "encoding/float_codec.h"
#include "encoding/int_codec.h"
#include "encoding/kernels.h"
#include "encoding/string_codec.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENCODING_ID_COUNT   SX_ENCODING_ID_COUNT
#define DEFAULT_VALUE_COUNT (1u << 20)
#define DEFAULT_MIN_SECONDS 0.25
#define MAX_THREADS         256u
#define MAX_DATASETS        64u
#define MAX_CSV_PATHS       1024u

typedef struct bench_options {
    uint32_t value_count;
    double min_seconds;
    unsigned threads;
    const char* datasets[MAX_DATASETS];
    size_t dataset_count;
    char* csv_paths[MAX_CSV_PATHS];
    size_t csv_path_count;
    bool columns_given;
    bench_csv_options csv;
} bench_options;

/* Every chunk encoded into its own slot, sized for the worst case so that chunks can be
 * encoded in parallel. */
typedef struct encoded_column {
    uint8_t* data;
    size_t* slot_offsets;
    size_t* sizes;
    sx_encoding* encodings;
} encoded_column;

/* Decode output buffers for one chunk; one set per thread. */
typedef struct decode_buffers {
    int64_t ints[SX_VECTOR_SIZE];
    double doubles[SX_VECTOR_SIZE];
    uint8_t bools[SX_VECTOR_SIZE];
    uint32_t offsets[SX_VECTOR_SIZE + 1];
    uint8_t* string_data;
    size_t string_capacity;
} decode_buffers;

/* One column being measured. */
typedef struct bench_job {
    const bench_column* column;
    const bench_chunks* chunks;
    encoded_column encoded;
    decode_buffers* buffers[MAX_THREADS];
    unsigned threads;
} bench_job;

typedef struct bench_result {
    uint64_t raw_bytes;
    uint64_t encoded_bytes; /* payloads plus NULL bitmaps */
    uint64_t null_bitmap_bytes;
    double encode_seconds; /* per pass over the whole column */
    double decode_seconds;
    uint32_t encoding_counts[ENCODING_ID_COUNT];
} bench_result;

/* ---------------------------------------------------------------------------------------- */
/* Encoding and decoding one chunk                                                          */
/* ---------------------------------------------------------------------------------------- */

static size_t chunk_bound(const bench_column* column, uint32_t start, uint32_t count) {
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

/*
 * Encodes one chunk. `double_encoder` carries ALP parameter choices from one chunk to the
 * next, as when the storage engine encodes the chunks of a column in order.
 */
static sx_err encode_chunk(const bench_column* column, uint32_t start, uint32_t count,
                           sx_double_encoder* double_encoder, sx_byte_writer* writer,
                           sx_encoding* encoding) {
    switch (column->type) {
        case BENCH_TYPE_INT64:
            return sx_int64_encode(column->ints + start, count, writer, encoding);
        case BENCH_TYPE_DOUBLE:
            return sx_double_encoder_encode(double_encoder, column->doubles + start, count, writer,
                                            encoding);
        case BENCH_TYPE_BOOL:
            return sx_bool_encode(column->bools + start, count, writer, encoding);
        case BENCH_TYPE_STRING:
            return sx_string_encode(column->offsets + start, column->string_data, count, writer,
                                    encoding);
    }
    return SX_ERR_UNSUPPORTED;
}

static sx_err decode_chunk(const bench_job* job, uint32_t chunk, decode_buffers* buffers) {
    const uint8_t* payload = job->encoded.data + job->encoded.slot_offsets[chunk];
    size_t size = job->encoded.sizes[chunk];
    sx_encoding encoding = job->encoded.encodings[chunk];
    uint32_t count = job->chunks->counts[chunk];
    switch (job->column->type) {
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

/* Compares a decoded chunk with the input. */
static bool chunk_matches(const bench_column* column, uint32_t start, uint32_t count,
                          const decode_buffers* buffers) {
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

/* ---------------------------------------------------------------------------------------- */
/* Passes over a range of chunks, optionally on several threads                             */
/* ---------------------------------------------------------------------------------------- */

static bool encode_range(bench_job* job, uint32_t first, uint32_t end) {
    sx_double_encoder double_encoder;
    sx_double_encoder_init(&double_encoder);
    for (uint32_t chunk = first; chunk < end; chunk++) {
        uint32_t start = job->chunks->starts[chunk];
        uint32_t count = job->chunks->counts[chunk];
        sx_byte_writer writer;
        sx_writer_init(&writer, job->encoded.data + job->encoded.slot_offsets[chunk],
                       chunk_bound(job->column, start, count));
        if (encode_chunk(job->column, start, count, &double_encoder, &writer,
                         &job->encoded.encodings[chunk]) != SX_OK) {
            fprintf(stderr, "error: encoding chunk %u failed\n", chunk);
            return false;
        }
        job->encoded.sizes[chunk] = writer.size;
    }
    return true;
}

static bool decode_range(bench_job* job, uint32_t first, uint32_t end, decode_buffers* buffers,
                         bool verify) {
    for (uint32_t chunk = first; chunk < end; chunk++) {
        sx_err err = decode_chunk(job, chunk, buffers);
        if (err != SX_OK) {
            fprintf(stderr, "error: decoding chunk %u failed: %s\n", chunk, sx_err_str(err));
            return false;
        }
        if (verify && !chunk_matches(job->column, job->chunks->starts[chunk],
                                     job->chunks->counts[chunk], buffers)) {
            fprintf(stderr, "error: chunk %u does not round-trip\n", chunk);
            return false;
        }
    }
    return true;
}

typedef struct worker_task {
    bench_job* job;
    uint32_t first;
    uint32_t end;
    decode_buffers* buffers;
    bool decode;
    bool verify;
    uint32_t passes;
    bool ok;
} worker_task;

/* Processes the task's chunks `passes` times, so threads start once per measurement. */
static void* worker_main(void* arg) {
    worker_task* task = arg;
    task->ok = true;
    for (uint32_t pass = 0; pass < task->passes && task->ok; pass++) {
        if (task->decode) {
            task->ok = decode_range(task->job, task->first, task->end, task->buffers, task->verify);
        } else {
            task->ok = encode_range(task->job, task->first, task->end);
        }
    }
    return NULL;
}

/* Runs `passes` encode or decode passes over every chunk, split evenly across the threads. */
static bool run_passes(bench_job* job, bool decode, bool verify, uint32_t passes) {
    uint32_t chunk_count = job->chunks->count;
    unsigned threads = job->threads;
    uint32_t per_thread = (chunk_count + threads - 1u) / threads;
    worker_task tasks[MAX_THREADS];
    pthread_t ids[MAX_THREADS];

    for (unsigned t = 0; t < threads; t++) {
        uint32_t first = t * per_thread < chunk_count ? t * per_thread : chunk_count;
        uint32_t end = first + per_thread < chunk_count ? first + per_thread : chunk_count;
        tasks[t] = (worker_task){
            .job = job,
            .first = first,
            .end = end,
            .buffers = job->buffers[t],
            .decode = decode,
            .verify = verify,
            .passes = passes,
        };
    }
    if (threads == 1) {
        worker_main(&tasks[0]);
        return tasks[0].ok;
    }
    for (unsigned t = 0; t < threads; t++) {
        if (pthread_create(&ids[t], NULL, worker_main, &tasks[t]) != 0) {
            fprintf(stderr, "error: cannot start thread %u\n", t);
            for (unsigned started = 0; started < t; started++) {
                pthread_join(ids[started], NULL);
            }
            return false;
        }
    }
    bool ok = true;
    for (unsigned t = 0; t < threads; t++) {
        pthread_join(ids[t], NULL);
        ok = ok && tasks[t].ok;
    }
    return ok;
}

static double seconds_since(uint64_t start_ns) {
    return (double)(sx_clock_monotonic_ns() - start_ns) / 1e9;
}

/*
 * Runs one untimed pass (which verifies decoding) and one timed pass to estimate the
 * cost, then times enough passes to fill `min_seconds` and reports the time of one pass.
 */
static bool time_passes(bench_job* job, bool decode, double min_seconds, double* seconds_per_pass) {
    if (!run_passes(job, decode, decode, 1)) {
        return false;
    }
    uint64_t start = sx_clock_monotonic_ns();
    if (!run_passes(job, decode, false, 1)) {
        return false;
    }
    double estimate = seconds_since(start);
    uint32_t passes = 1;
    if (estimate < min_seconds) {
        double wanted = min_seconds / (estimate > 1e-6 ? estimate : 1e-6);
        passes = wanted > 100000.0 ? 100000u : (uint32_t)wanted + 1u;
    }
    start = sx_clock_monotonic_ns();
    if (!run_passes(job, decode, false, passes)) {
        return false;
    }
    *seconds_per_pass = seconds_since(start) / passes;
    return true;
}

/* ---------------------------------------------------------------------------------------- */
/* Setting up and running one column                                                        */
/* ---------------------------------------------------------------------------------------- */

static bool job_init(bench_job* job, const bench_column* column, const bench_chunks* chunks,
                     unsigned threads) {
    memset(job, 0, sizeof(*job));
    job->column = column;
    job->chunks = chunks;
    job->threads = threads;

    encoded_column* encoded = &job->encoded;
    encoded->slot_offsets = calloc(chunks->count, sizeof(size_t));
    encoded->sizes = calloc(chunks->count, sizeof(size_t));
    encoded->encodings = calloc(chunks->count, sizeof(sx_encoding));
    if (encoded->slot_offsets == NULL || encoded->sizes == NULL || encoded->encodings == NULL) {
        return false;
    }

    size_t capacity = 0;
    size_t max_string_bytes = 1;
    for (uint32_t chunk = 0; chunk < chunks->count; chunk++) {
        uint32_t start = chunks->starts[chunk];
        uint32_t count = chunks->counts[chunk];
        encoded->slot_offsets[chunk] = capacity;
        capacity += sx_align_up(chunk_bound(column, start, count), 8);
        if (column->type == BENCH_TYPE_STRING) {
            size_t bytes = column->offsets[start + count] - column->offsets[start];
            max_string_bytes = bytes > max_string_bytes ? bytes : max_string_bytes;
        }
    }
    encoded->data = malloc(capacity > 0 ? capacity : 1);
    if (encoded->data == NULL) {
        return false;
    }

    for (unsigned t = 0; t < threads; t++) {
        job->buffers[t] = calloc(1, sizeof(decode_buffers));
        if (job->buffers[t] == NULL) {
            return false;
        }
        if (column->type == BENCH_TYPE_STRING) {
            job->buffers[t]->string_capacity = max_string_bytes;
            job->buffers[t]->string_data = malloc(max_string_bytes);
            if (job->buffers[t]->string_data == NULL) {
                return false;
            }
        }
    }
    return true;
}

static void job_free(bench_job* job) {
    free(job->encoded.data);
    free(job->encoded.slot_offsets);
    free(job->encoded.sizes);
    free(job->encoded.encodings);
    for (unsigned t = 0; t < job->threads; t++) {
        if (job->buffers[t] != NULL) {
            free(job->buffers[t]->string_data);
            free(job->buffers[t]);
        }
    }
}

/* Bytes the storage engine needs for NULL bitmaps: one per chunk that contains a NULL. */
static uint64_t null_bitmap_bytes(const bench_column* column, const bench_chunks* chunks) {
    if (column->nulls == NULL) {
        return 0;
    }
    uint64_t bytes = 0;
    for (uint32_t chunk = 0; chunk < chunks->count; chunk++) {
        uint32_t start = chunks->starts[chunk];
        uint32_t count = chunks->counts[chunk];
        for (uint32_t i = 0; i < count; i++) {
            if (column->nulls[start + i]) {
                bytes += sx_div_round_up(count, 64) * 8u;
                break;
            }
        }
    }
    return bytes;
}

static bool run_benchmark(const bench_column* column, const bench_chunks* chunks,
                          const bench_options* options, bench_result* result) {
    memset(result, 0, sizeof(*result));
    bench_job job;
    bool ok = job_init(&job, column, chunks, options->threads);
    ok = ok && time_passes(&job, false, options->min_seconds, &result->encode_seconds);
    ok = ok && time_passes(&job, true, options->min_seconds, &result->decode_seconds);
    if (ok) {
        result->raw_bytes = bench_column_raw_bytes(column);
        result->null_bitmap_bytes = null_bitmap_bytes(column, chunks);
        result->encoded_bytes = result->null_bitmap_bytes;
        for (uint32_t chunk = 0; chunk < chunks->count; chunk++) {
            result->encoded_bytes += job.encoded.sizes[chunk];
            result->encoding_counts[job.encoded.encodings[chunk]]++;
        }
    }
    job_free(&job);
    return ok;
}

/* ---------------------------------------------------------------------------------------- */
/* Reporting                                                                                */
/* ---------------------------------------------------------------------------------------- */

static void print_header(const char* first_column) {
    printf("%-18s %-7s %10s %9s %8s %10s %10s %10s  %s\n", first_column, "type", "values",
           "bytes/val", "ratio", "enc MB/s", "dec MB/s", "dec Mval/s", "encodings");
    printf("%-18s %-7s %10s %9s %8s %10s %10s %10s  %s\n", "------------------", "-------",
           "----------", "---------", "--------", "----------", "----------", "----------",
           "---------");
}

/* Lists the encodings used, most frequent first, with their share of chunks. */
static void format_encodings(const bench_result* result, uint32_t chunk_count, char* out,
                             size_t size) {
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
        double share = 100.0 * result->encoding_counts[best] / chunk_count;
        int n = snprintf(out + written, size - written, "%s%s %.0f%%", written > 0 ? ", " : "",
                         sx_encoding_name((sx_encoding)best), share);
        if (n < 0) {
            return;
        }
        written += (size_t)n;
    }
}

static void print_result(const char* name, const bench_column* column, uint32_t chunk_count,
                         const bench_result* result) {
    double bytes_per_value = (double)result->encoded_bytes / column->count;
    double ratio = (double)result->raw_bytes / (double)result->encoded_bytes;
    double encode_mb = (double)result->raw_bytes / result->encode_seconds / 1e6;
    double decode_mb = (double)result->raw_bytes / result->decode_seconds / 1e6;
    double decode_mvalues = (double)column->count / result->decode_seconds / 1e6;
    char encodings[160];
    format_encodings(result, chunk_count, encodings, sizeof(encodings));
    printf("%-18s %-7s %10u %9.3f %7.1fx %10.0f %10.0f %10.0f  %s\n", name,
           bench_type_name(column->type), column->count, bytes_per_value, ratio, encode_mb,
           decode_mb, decode_mvalues, encodings);
}

/* ---------------------------------------------------------------------------------------- */
/* Synthetic datasets                                                                       */
/* ---------------------------------------------------------------------------------------- */

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
    bench_chunks chunks = {0};
    bool ok = bench_column_init(&column, dataset->type, options->value_count);
    if (ok) {
        column.count = options->value_count;
        ok = dataset->generate(&column, 42) &&
             bench_chunks_fixed(&chunks, column.count, SX_VECTOR_SIZE);
    }
    bench_result result;
    ok = ok && run_benchmark(&column, &chunks, options, &result);
    if (ok) {
        print_result(dataset->name, &column, chunks.count, &result);
    } else {
        fprintf(stderr, "error: dataset %s failed\n", dataset->name);
    }
    bench_chunks_free(&chunks);
    bench_column_free(&column);
    return ok;
}

static void print_settings(const bench_options* options) {
    printf("threads: %u, kernels: %s\n\n", options->threads,
           sx_simd_level_name(sx_codec_kernels_active()->level));
}

static int run_datasets(const bench_options* options) {
    print_settings(options);
    print_header("dataset");
    bool ok = true;
    for (size_t i = 0; i < options->dataset_count; i++) {
        const bench_dataset* dataset = find_dataset(options->datasets[i]);
        if (dataset == NULL) {
            fprintf(stderr, "error: unknown dataset %s (see --list)\n", options->datasets[i]);
            return 2;
        }
        ok = run_dataset(dataset, options) && ok;
    }
    if (options->dataset_count == 0) {
        for (size_t i = 0; i < bench_dataset_count; i++) {
            ok = run_dataset(&bench_datasets[i], options) && ok;
        }
    }
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------------------------------------- */
/* CSV input                                                                                */
/* ---------------------------------------------------------------------------------------- */

static void print_csv_summary(const bench_csv_data* data, double load_seconds) {
    uint32_t rows = data->columns[0].count;
    printf("input:   %u files, %u rows, %.1f MB of CSV (loaded in %.1f s)\n", data->file_count,
           rows, (double)data->input_bytes / 1e6, load_seconds);
    printf("chunks:  %u, %.0f rows per chunk on average\n", data->chunks.count,
           (double)rows / data->chunks.count);
}

static int run_csv(const bench_options* options) {
    bench_csv_data data;
    char error[1024];
    uint64_t load_start = sx_clock_monotonic_ns();
    if (!bench_load_csv(options->csv_paths, options->csv_path_count, &options->csv, &data, error,
                        sizeof(error))) {
        fprintf(stderr, "error: %s\n", error);
        return 1;
    }
    print_csv_summary(&data, seconds_since(load_start));
    print_settings(options);
    print_header("column");

    bool ok = true;
    uint64_t total_encoded = 0;
    for (size_t c = 0; c < data.column_count && ok; c++) {
        bench_result result;
        ok = run_benchmark(&data.columns[c], &data.chunks, options, &result);
        if (ok) {
            print_result(options->csv.columns[c].label, &data.columns[c], data.chunks.count,
                         &result);
            total_encoded += result.encoded_bytes;
            if (data.columns[c].null_count > 0) {
                printf("%-18s   %llu NULLs, %llu bytes of NULL bitmaps included above\n", "",
                       (unsigned long long)data.columns[c].null_count,
                       (unsigned long long)result.null_bitmap_bytes);
            }
        }
    }
    if (ok) {
        uint32_t rows = data.columns[0].count;
        printf("\ntotal:   %.1f MB encoded for these columns, %.2f bytes per row "
               "(%.1fx smaller than the CSV files)\n",
               (double)total_encoded / 1e6, (double)total_encoded / rows,
               (double)data.input_bytes / (double)total_encoded);
    }
    bench_csv_data_free(&data);
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------------------------------------- */
/* Command line                                                                             */
/* ---------------------------------------------------------------------------------------- */

static void print_usage(void) {
    printf("usage: sxdb-bench [options] [DATASET ...]\n"
           "       sxdb-bench [options] --csv PATH [PATH ...] --columns FIELD:TYPE[,...]\n"
           "                  [--skip-header] [--split-on FIELD]\n"
           "       sxdb-bench --list\n"
           "\n"
           "options:\n"
           "  --values N      values per synthetic dataset (default %u)\n"
           "  --min-time S    minimum seconds per measurement (default %.2f)\n"
           "  --threads N     encode and decode with N threads (default 1)\n"
           "  --simd LEVEL    codec kernels: scalar, avx2, avx512, or neon (default: the\n"
           "                  best the CPU supports)\n"
           "\n"
           "CSV input: PATH is a file or a directory (every file in it). Each file is one\n"
           "series. TYPE is int64, double, bool, string, timestamp, or decimalN (N = 0..18\n"
           "fraction digits). --split-on starts a new chunk when that field changes.\n",
           DEFAULT_VALUE_COUNT, DEFAULT_MIN_SECONDS);
}

static void print_datasets(void) {
    for (size_t i = 0; i < bench_dataset_count; i++) {
        printf("%-18s %-7s %s\n", bench_datasets[i].name, bench_type_name(bench_datasets[i].type),
               bench_datasets[i].description);
    }
}

/* Parses the command line. Returns -1 to continue, or an exit status. */
static int parse_arguments(int argc, char** argv, bench_options* options) {
    bool in_csv_paths = false;
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        bool has_value = i + 1 < argc;
        if (arg[0] != '-' && in_csv_paths && options->csv_path_count < MAX_CSV_PATHS) {
            options->csv_paths[options->csv_path_count] = argv[i];
            options->csv_path_count++;
            continue;
        }
        in_csv_paths = false;
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
            options->value_count = (uint32_t)value;
        } else if (strcmp(arg, "--min-time") == 0 && has_value) {
            options->min_seconds = strtod(argv[++i], NULL);
        } else if (strcmp(arg, "--threads") == 0 && has_value) {
            long value = strtol(argv[++i], NULL, 10);
            if (value < 1 || value > (long)MAX_THREADS) {
                fprintf(stderr, "error: --threads must be between 1 and %u\n", MAX_THREADS);
                return 2;
            }
            options->threads = (unsigned)value;
        } else if (strcmp(arg, "--simd") == 0 && has_value) {
            sx_simd_level level;
            const char* name = argv[++i];
            if (!sx_simd_level_from_name(name, &level)) {
                fprintf(stderr, "error: unknown kernel set %s\n", name);
                return 2;
            }
            if (!sx_simd_set_level(level)) {
                fprintf(stderr, "error: kernel set %s is not supported on this machine\n", name);
                return 2;
            }
        } else if (strcmp(arg, "--csv") == 0) {
            in_csv_paths = true;
        } else if (strcmp(arg, "--columns") == 0 && has_value) {
            char error[256];
            if (!bench_parse_column_specs(argv[++i], &options->csv, error, sizeof(error))) {
                fprintf(stderr, "error: %s\n", error);
                return 2;
            }
            options->columns_given = true;
        } else if (strcmp(arg, "--skip-header") == 0) {
            options->csv.skip_header = true;
        } else if (strcmp(arg, "--split-on") == 0 && has_value) {
            options->csv.split = true;
            options->csv.split_field = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (arg[0] != '-' && options->dataset_count < MAX_DATASETS) {
            options->datasets[options->dataset_count] = arg;
            options->dataset_count++;
        } else {
            print_usage();
            return 2;
        }
    }
    return -1;
}

int main(int argc, char** argv) {
    static bench_options options;
    options.value_count = DEFAULT_VALUE_COUNT;
    options.min_seconds = DEFAULT_MIN_SECONDS;
    options.threads = 1;

    int status = parse_arguments(argc, argv, &options);
    if (status >= 0) {
        return status;
    }
    if (options.csv_path_count > 0) {
        if (!options.columns_given) {
            fprintf(stderr, "error: --csv needs --columns\n");
            return 2;
        }
        return run_csv(&options);
    }
    return run_datasets(&options);
}
