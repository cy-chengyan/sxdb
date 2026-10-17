/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * An in-memory column of benchmark input: one of 64-bit integers, doubles, booleans, or
 * strings (Arrow layout: offsets plus bytes), with optional NULL flags.
 */

#ifndef SXDB_BENCH_COLUMN_H
#define SXDB_BENCH_COLUMN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum bench_type {
    BENCH_TYPE_INT64,
    BENCH_TYPE_DOUBLE,
    BENCH_TYPE_BOOL,
    BENCH_TYPE_STRING,
} bench_type;

typedef struct bench_column {
    bench_type type;
    uint32_t count;    /* values stored */
    uint32_t capacity; /* values allocated */
    int64_t* ints;
    double* doubles;
    uint8_t* bools;
    uint32_t* offsets;    /* capacity + 1 entries */
    uint8_t* string_data; /* string bytes */
    size_t string_capacity;
    uint8_t* nulls; /* 1 where the input was NULL; allocated on the first NULL */
    uint64_t null_count;
} bench_column;

/* Allocates room for `capacity` values; the column starts empty. */
bool bench_column_init(bench_column* column, bench_type type, uint32_t capacity);

/* Grows the column so it can hold at least `capacity` values. */
bool bench_column_reserve(bench_column* column, uint32_t capacity);

/* Stores string `index`. Strings must be stored in index order. */
bool bench_column_set_string(bench_column* column, uint32_t index, const char* text, size_t length);

/* Records that value `index` was NULL in the input. */
bool bench_column_mark_null(bench_column* column, uint32_t index);

void bench_column_free(bench_column* column);

const char* bench_type_name(bench_type type);

/* Size of the column in its uncompressed in-memory form, for compression ratios. */
uint64_t bench_column_raw_bytes(const bench_column* column);

#endif /* SXDB_BENCH_COLUMN_H */
