/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "column.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_STRING_CAPACITY ((size_t)1 << 20)

/* Resizes *array to hold `count` elements of `size` bytes; new elements are zeroed. */
static bool resize_array(void** array, size_t old_count, size_t count, size_t size) {
    void* resized = realloc(*array, count * size);
    if (resized == NULL) {
        return false;
    }
    if (count > old_count) {
        memset((uint8_t*)resized + old_count * size, 0, (count - old_count) * size);
    }
    *array = resized;
    return true;
}

bool bench_column_init(bench_column* column, bench_type type, uint32_t capacity) {
    memset(column, 0, sizeof(*column));
    column->type = type;
    if (type == BENCH_TYPE_STRING) {
        column->string_capacity = INITIAL_STRING_CAPACITY;
        column->string_data = malloc(column->string_capacity);
        if (column->string_data == NULL) {
            return false;
        }
    }
    return bench_column_reserve(column, capacity > 0 ? capacity : 1);
}

bool bench_column_reserve(bench_column* column, uint32_t capacity) {
    if (capacity <= column->capacity) {
        return true;
    }
    size_t old = column->capacity;
    bool ok = true;
    switch (column->type) {
        case BENCH_TYPE_INT64:
            ok = resize_array((void**)&column->ints, old, capacity, sizeof(int64_t));
            break;
        case BENCH_TYPE_DOUBLE:
            ok = resize_array((void**)&column->doubles, old, capacity, sizeof(double));
            break;
        case BENCH_TYPE_BOOL:
            ok = resize_array((void**)&column->bools, old, capacity, sizeof(uint8_t));
            break;
        case BENCH_TYPE_STRING: {
            size_t old_offsets = column->offsets == NULL ? 0 : old + 1u;
            ok = resize_array((void**)&column->offsets, old_offsets, (size_t)capacity + 1u,
                              sizeof(uint32_t));
            break;
        }
    }
    if (ok && column->nulls != NULL) {
        ok = resize_array((void**)&column->nulls, old, capacity, sizeof(uint8_t));
    }
    if (ok) {
        column->capacity = capacity;
    }
    return ok;
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

bool bench_column_mark_null(bench_column* column, uint32_t index) {
    if (column->nulls == NULL) {
        column->nulls = calloc(column->capacity, sizeof(uint8_t));
        if (column->nulls == NULL) {
            return false;
        }
    }
    column->nulls[index] = 1;
    column->null_count++;
    return true;
}

void bench_column_free(bench_column* column) {
    free(column->ints);
    free(column->doubles);
    free(column->bools);
    free(column->offsets);
    free(column->string_data);
    free(column->nulls);
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
