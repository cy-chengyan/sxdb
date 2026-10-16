/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "csv.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 65536

/* Finds field `index` of a comma-separated line; returns its start and sets its length. */
static const char* find_field(const char* line, unsigned index, size_t* length) {
    const char* start = line;
    for (unsigned i = 0; i < index; i++) {
        start = strchr(start, ',');
        if (start == NULL) {
            return NULL;
        }
        start++;
    }
    *length = strcspn(start, ",\r\n");
    return start;
}

static bool parse_int(const char* text, size_t length, int64_t* value) {
    char buffer[64];
    if (length == 0 || length >= sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    char* end = NULL;
    errno = 0;
    long long parsed = strtoll(buffer, &end, 10);
    if (errno != 0 || *end != '\0') {
        return false;
    }
    *value = (int64_t)parsed;
    return true;
}

static bool parse_double(const char* text, size_t length, double* value) {
    char buffer[64];
    if (length == 0 || length >= sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    char* end = NULL;
    *value = strtod(buffer, &end);
    return *end == '\0';
}

static bool parse_bool(const char* text, size_t length, uint8_t* value) {
    if ((length == 1 && text[0] == '1') || (length == 4 && strncmp(text, "true", 4) == 0)) {
        *value = 1;
        return true;
    }
    if ((length == 1 && text[0] == '0') || (length == 5 && strncmp(text, "false", 5) == 0)) {
        *value = 0;
        return true;
    }
    return false;
}

/* Grows the column so it can hold at least `count` values. */
static bool reserve_values(bench_column* column, uint32_t count, uint32_t* capacity) {
    if (count <= *capacity) {
        return true;
    }
    uint32_t grown = *capacity * 2u;
    void* resized = NULL;
    switch (column->type) {
        case BENCH_TYPE_INT64:
            resized = realloc(column->ints, (size_t)grown * sizeof(int64_t));
            column->ints = resized != NULL ? resized : column->ints;
            break;
        case BENCH_TYPE_DOUBLE:
            resized = realloc(column->doubles, (size_t)grown * sizeof(double));
            column->doubles = resized != NULL ? resized : column->doubles;
            break;
        case BENCH_TYPE_BOOL:
            resized = realloc(column->bools, grown);
            column->bools = resized != NULL ? resized : column->bools;
            break;
        case BENCH_TYPE_STRING:
            resized = realloc(column->offsets, ((size_t)grown + 1u) * sizeof(uint32_t));
            column->offsets = resized != NULL ? resized : column->offsets;
            break;
    }
    if (resized == NULL) {
        return false;
    }
    *capacity = grown;
    return true;
}

static bool store_field(bench_column* column, uint32_t index, const char* field, size_t length) {
    switch (column->type) {
        case BENCH_TYPE_INT64:
            return parse_int(field, length, &column->ints[index]);
        case BENCH_TYPE_DOUBLE:
            return parse_double(field, length, &column->doubles[index]);
        case BENCH_TYPE_BOOL:
            return parse_bool(field, length, &column->bools[index]);
        case BENCH_TYPE_STRING:
            return bench_column_set_string(column, index, field, length);
    }
    return false;
}

bool bench_load_csv(const char* path, unsigned column_index, bench_type type, bool skip_header,
                    bench_column* out, char* error, size_t error_size) {
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        snprintf(error, error_size, "cannot open %s: %s", path, strerror(errno));
        return false;
    }
    uint32_t capacity = 4096;
    if (!bench_column_init(out, type, capacity, 1u << 20)) {
        fclose(file);
        snprintf(error, error_size, "out of memory");
        return false;
    }

    static char line[MAX_LINE];
    uint32_t count = 0;
    unsigned long line_number = 0;
    bool ok = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        if (skip_header && line_number == 1) {
            continue;
        }
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') {
            continue;
        }
        size_t length = 0;
        const char* field = find_field(line, column_index, &length);
        if (field == NULL) {
            snprintf(error, error_size, "line %lu has no column %u", line_number, column_index);
            ok = false;
            break;
        }
        if (count == UINT32_MAX - 1u || !reserve_values(out, count + 1u, &capacity)) {
            snprintf(error, error_size, "too many rows or out of memory");
            ok = false;
            break;
        }
        if (!store_field(out, count, field, length)) {
            snprintf(error, error_size, "line %lu: cannot parse \"%.*s\" as %s", line_number,
                     (int)length, field, bench_type_name(type));
            ok = false;
            break;
        }
        count++;
    }
    fclose(file);

    if (ok && count == 0) {
        snprintf(error, error_size, "%s has no data rows", path);
        ok = false;
    }
    if (!ok) {
        bench_column_free(out);
        return false;
    }
    out->count = count;
    return true;
}
