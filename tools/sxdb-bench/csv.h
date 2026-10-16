/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Loads one column of a CSV file as benchmark input, so encodings can be measured on
 * real data. Fields are separated by commas; quoting is not supported.
 */

#ifndef SXDB_BENCH_CSV_H
#define SXDB_BENCH_CSV_H

#include "column.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Reads column `column_index` (0-based) of every row of `path` into `out`. Rows that are
 * too short are an error; with `skip_header` the first row is ignored. On failure writes
 * a message to `error`.
 */
bool bench_load_csv(const char* path, unsigned column_index, bench_type type, bool skip_header,
                    bench_column* out, char* error, size_t error_size);

#endif /* SXDB_BENCH_CSV_H */
