/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Loads columns of real data from CSV files as benchmark input.
 *
 * Every file is treated as one series, so chunks never span two files. Optionally a
 * field such as a trading date can mark partitions: a new chunk starts whenever its
 * value changes, the way a DAY-partitioned table cuts blocks.
 *
 * Fields are separated by commas; quoting is not supported. An empty field is NULL. The
 * NULL is recorded and the value is filled from the previous row of the same file (or
 * the next one, at the start of a file), which is how the storage engine fills NULL
 * slots so they do not widen an encoding's value range.
 */

#ifndef SXDB_BENCH_CSV_H
#define SXDB_BENCH_CSV_H

#include "chunks.h"
#include "column.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BENCH_MAX_CSV_COLUMNS 32u

typedef enum bench_value_kind {
    BENCH_VALUE_INT64,     /* integer text */
    BENCH_VALUE_DOUBLE,    /* floating-point text */
    BENCH_VALUE_BOOL,      /* 0, 1, true, false */
    BENCH_VALUE_STRING,    /* any text */
    BENCH_VALUE_TIMESTAMP, /* date and time, stored as nanoseconds since the epoch */
    BENCH_VALUE_DECIMAL,   /* exact decimal, stored as an integer scaled by 10^scale */
} bench_value_kind;

typedef struct bench_column_spec {
    unsigned field; /* 0-based CSV field */
    bench_value_kind kind;
    unsigned scale; /* fraction digits kept by BENCH_VALUE_DECIMAL */
    char label[48]; /* as written on the command line, for reports */
} bench_column_spec;

typedef struct bench_csv_options {
    bench_column_spec columns[BENCH_MAX_CSV_COLUMNS];
    size_t column_count;
    bool skip_header; /* ignore the first line of every file */
    bool split;       /* start a new chunk whenever field `split_field` changes */
    unsigned split_field;
    uint32_t chunk_rows; /* most rows per chunk, 1 to SX_VECTOR_SIZE; 0 means SX_VECTOR_SIZE */
} bench_csv_options;

typedef struct bench_csv_data {
    bench_column columns[BENCH_MAX_CSV_COLUMNS];
    size_t column_count;
    bench_chunks chunks;
    uint32_t file_count;
    uint64_t input_bytes; /* total size of the CSV files */
} bench_csv_data;

/*
 * Parses a column list such as "2:timestamp,3:double,7:decimal0". Types are int64,
 * double, bool, string, timestamp, and decimalN with N from 0 to 18.
 */
bool bench_parse_column_specs(const char* text, bench_csv_options* options, char* error,
                              size_t error_size);

/*
 * Loads the configured columns from every path; a directory stands for all regular files
 * in it, in name order. On failure writes a message to `error`.
 */
bool bench_load_csv(char* const* paths, size_t path_count, const bench_csv_options* options,
                    bench_csv_data* data, char* error, size_t error_size);

void bench_csv_data_free(bench_csv_data* data);

#endif /* SXDB_BENCH_CSV_H */
