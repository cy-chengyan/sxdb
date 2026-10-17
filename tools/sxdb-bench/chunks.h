/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * How a column is cut into chunks, mirroring how the storage engine cuts blocks: at most
 * 1024 values each, never spanning two series, and optionally never spanning two
 * partitions (for example two trading days).
 */

#ifndef SXDB_BENCH_CHUNKS_H
#define SXDB_BENCH_CHUNKS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct bench_chunks {
    uint32_t* starts;
    uint32_t* counts;
    uint32_t count;
    uint32_t capacity;
} bench_chunks;

/* Appends a chunk of `count` values starting at value `start`. */
bool bench_chunks_add(bench_chunks* chunks, uint32_t start, uint32_t count);

/* Cuts `total` values into consecutive chunks of `size` values (the last may be shorter). */
bool bench_chunks_fixed(bench_chunks* chunks, uint32_t total, uint32_t size);

void bench_chunks_free(bench_chunks* chunks);

#endif /* SXDB_BENCH_CHUNKS_H */
