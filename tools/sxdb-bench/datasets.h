/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Synthetic datasets that imitate common time-series columns: timestamps, counters,
 * prices, sensor readings, flags, and identifiers. They are deterministic (fixed seeds)
 * so results are comparable between runs. Real data can be benchmarked with --csv.
 */

#ifndef SXDB_BENCH_DATASETS_H
#define SXDB_BENCH_DATASETS_H

#include "column.h"

#include <stddef.h>
#include <stdint.h>

typedef struct bench_dataset {
    const char* name;
    const char* description;
    bench_type type;
    /* Fills an initialized column of the dataset's type with `column->count` values. */
    bool (*generate)(bench_column* column, uint64_t seed);
} bench_dataset;

extern const bench_dataset bench_datasets[];
extern const size_t bench_dataset_count;

#endif /* SXDB_BENCH_DATASETS_H */
