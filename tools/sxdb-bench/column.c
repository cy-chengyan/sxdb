/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "column.h"

#include <stdlib.h>
#include <string.h>

bool bench_column_init(bench_column* column, bench_type type, uint32_t count,
                       size_t string_capacity) {
    memset(column, 0, sizeof(*column));
    column->type = type;
    column->count = count;
    switch (type) {
        case BENCH_TYPE_INT64:
            column->ints = calloc(count, sizeof(int64_t));
            return column->ints != NULL;
        case BENCH_TYPE_DOUBLE:
            column->doubles = calloc(count, sizeof(double));
            return column->doubles != NULL;
        case BENCH_TYPE_BOOL:
            column->bools = calloc(count, sizeof(uint8_t));
            return column->bools != NULL;
        case BENCH_TYPE_STRING:
            column->offsets = calloc((size_t)count + 1u, sizeof(uint32_t));
            column->string_capacity = string_capacity > 0 ? string_capacity : 1024;
            column->string_data = malloc(column->string_capacity);
            return column->offsets != NULL && column->string_data != NULL;
    }
    return false;
}

bool bench_column_set_string(bench_column* column, uint32_t index, const char* text,
                             size_t length) {
    uint32_t start = column->offsets[index];
    size_t needed = (size_t)start + length;
    if (needed > UINT32_MAX) {
        return false;
    }
    if (needed > column->string_capacity) {
        size_t capacity = column->string_capacity;
        while (capacity < needed) {
            capacity *= 2;
        }
        uint8_t* grown = realloc(column->string_data, capacity);
        if (grown == NULL) {
            return false;
        }
        column->string_data = grown;
        column->string_capacity = capacity;
    }
    if (length > 0) {
        memcpy(column->string_data + start, text, length);
    }
    column->offsets[index + 1] = (uint32_t)needed;
    return true;
}

void bench_column_free(bench_column* column) {
    free(column->ints);
    free(column->doubles);
    free(column->bools);
    free(column->offsets);
    free(column->string_data);
    memset(column, 0, sizeof(*column));
}

const char* bench_type_name(bench_type type) {
    switch (type) {
        case BENCH_TYPE_INT64:
            return "int64";
        case BENCH_TYPE_DOUBLE:
            return "double";
        case BENCH_TYPE_BOOL:
            return "bool";
        case BENCH_TYPE_STRING:
            return "string";
    }
    return "unknown";
}

uint64_t bench_column_raw_bytes(const bench_column* column) {
    switch (column->type) {
        case BENCH_TYPE_INT64:
        case BENCH_TYPE_DOUBLE:
            return (uint64_t)column->count * 8u;
        case BENCH_TYPE_BOOL:
            return column->count;
        case BENCH_TYPE_STRING:
            return (uint64_t)column->offsets[column->count] + (uint64_t)column->count * 4u;
    }
    return 0;
}
