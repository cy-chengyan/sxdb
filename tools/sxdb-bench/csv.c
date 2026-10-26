/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "csv.h"

#include "common/compiler.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_LINE          (1u << 16)
#define MAX_FIELDS        256u
#define MAX_SPLIT_VALUE   256u
#define MAX_DECIMAL_SCALE 18u
#define NANOS_PER_SECOND  INT64_C(1000000000)

/* ---------------------------------------------------------------------------------------- */
/* Column specifications                                                                    */
/* ---------------------------------------------------------------------------------------- */

static bool parse_kind(const char* text, bench_column_spec* spec) {
    static const struct {
        const char* name;
        bench_value_kind kind;
    } kinds[] = {
        {"int64", BENCH_VALUE_INT64},         {"double", BENCH_VALUE_DOUBLE},
        {"bool", BENCH_VALUE_BOOL},           {"string", BENCH_VALUE_STRING},
        {"timestamp", BENCH_VALUE_TIMESTAMP},
    };
    for (size_t i = 0; i < SX_ARRAY_SIZE(kinds); i++) {
        if (strcmp(text, kinds[i].name) == 0) {
            spec->kind = kinds[i].kind;
            return true;
        }
    }
    if (strncmp(text, "decimal", 7) == 0 && text[7] >= '0' && text[7] <= '9') {
        char* end = NULL;
        unsigned long scale = strtoul(text + 7, &end, 10);
        if (*end != '\0' || scale > MAX_DECIMAL_SCALE) {
            return false;
        }
        spec->kind = BENCH_VALUE_DECIMAL;
        spec->scale = (unsigned)scale;
        return true;
    }
    return false;
}

bool bench_parse_column_specs(const char* text, bench_csv_options* options, char* error,
                              size_t error_size) {
    char buffer[1024];
    if (strlen(text) >= sizeof(buffer)) {
        snprintf(error, error_size, "column list is too long");
        return false;
    }
    snprintf(buffer, sizeof(buffer), "%s", text);

    options->column_count = 0;
    char* save = NULL;
    for (char* item = strtok_r(buffer, ",", &save); item != NULL;
         item = strtok_r(NULL, ",", &save)) {
        if (options->column_count == BENCH_MAX_CSV_COLUMNS) {
            snprintf(error, error_size, "at most %u columns", BENCH_MAX_CSV_COLUMNS);
            return false;
        }
        bench_column_spec* spec = &options->columns[options->column_count];
        memset(spec, 0, sizeof(*spec));
        char* colon = strchr(item, ':');
        char* end = NULL;
        unsigned long field = colon == NULL ? 0 : strtoul(item, &end, 10);
        if (colon == NULL || end != colon || field >= MAX_FIELDS || !parse_kind(colon + 1, spec)) {
            snprintf(error, error_size, "bad column \"%s\" (expected FIELD:TYPE)", item);
            return false;
        }
        spec->field = (unsigned)field;
        snprintf(spec->label, sizeof(spec->label), "%s", item);
        options->column_count++;
    }
    if (options->column_count == 0) {
        snprintf(error, error_size, "no columns given");
        return false;
    }
    return true;
}

static bench_type storage_type(bench_value_kind kind) {
    switch (kind) {
        case BENCH_VALUE_DOUBLE:
            return BENCH_TYPE_DOUBLE;
        case BENCH_VALUE_BOOL:
            return BENCH_TYPE_BOOL;
        case BENCH_VALUE_STRING:
            return BENCH_TYPE_STRING;
        case BENCH_VALUE_INT64:
        case BENCH_VALUE_TIMESTAMP:
        case BENCH_VALUE_DECIMAL:
            return BENCH_TYPE_INT64;
    }
    return BENCH_TYPE_INT64;
}

/* ---------------------------------------------------------------------------------------- */
/* Field parsing                                                                            */
/* ---------------------------------------------------------------------------------------- */

typedef struct field_ref {
    const char* text;
    size_t length;
} field_ref;

/* Splits a line at commas. Returns the number of fields. */
static size_t split_fields(const char* line, field_ref* fields, size_t max_fields) {
    size_t count = 0;
    const char* start = line;
    for (const char* p = line;; p++) {
        bool end_of_line = *p == '\0' || *p == '\n' || *p == '\r';
        if (*p != ',' && !end_of_line) {
            continue;
        }
        if (count < max_fields) {
            fields[count].text = start;
            fields[count].length = (size_t)(p - start);
            count++;
        }
        if (end_of_line) {
            return count;
        }
        start = p + 1;
    }
}

/* Copies a field into a NUL-terminated buffer for the strto* functions. */
static bool field_to_text(field_ref field, char* buffer, size_t size) {
    if (field.length == 0 || field.length >= size) {
        return false;
    }
    memcpy(buffer, field.text, field.length);
    buffer[field.length] = '\0';
    return true;
}

static bool parse_int64(field_ref field, int64_t* value) {
    char text[64];
    if (!field_to_text(field, text, sizeof(text))) {
        return false;
    }
    char* end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    if (errno != 0 || *end != '\0') {
        return false;
    }
    *value = (int64_t)parsed;
    return true;
}

static bool parse_double(field_ref field, double* value) {
    char text[64];
    if (!field_to_text(field, text, sizeof(text))) {
        return false;
    }
    char* end = NULL;
    *value = strtod(text, &end);
    return *end == '\0';
}

static bool parse_bool(field_ref field, uint8_t* value) {
    static const struct {
        const char* text;
        uint8_t value;
    } words[] = {{"1", 1}, {"0", 0}, {"true", 1}, {"false", 0}};
    for (size_t i = 0; i < SX_ARRAY_SIZE(words); i++) {
        if (field.length == strlen(words[i].text) &&
            memcmp(field.text, words[i].text, field.length) == 0) {
            *value = words[i].value;
            return true;
        }
    }
    return false;
}

/*
 * Parses an exact decimal such as "-1295.05" into an integer scaled by 10^scale.
 * Fraction digits beyond `scale` must be zero, so no value is silently rounded.
 */
static bool parse_decimal(field_ref field, unsigned scale, int64_t* value) {
    const char* p = field.text;
    const char* end = field.text + field.length;
    bool negative = p < end && *p == '-';
    if (p < end && (*p == '-' || *p == '+')) {
        p++;
    }

    uint64_t magnitude = 0;
    unsigned fraction_digits = 0;
    bool seen_digit = false;
    bool seen_point = false;
    for (; p < end; p++) {
        if (*p == '.' && !seen_point) {
            seen_point = true;
            continue;
        }
        if (*p < '0' || *p > '9') {
            return false;
        }
        unsigned digit = (unsigned)(*p - '0');
        seen_digit = true;
        if (seen_point && fraction_digits == scale) {
            if (digit != 0) {
                return false; /* would need more than `scale` decimal places */
            }
            continue;
        }
        if (magnitude > (UINT64_C(0x7FFFFFFFFFFFFFFF) - digit) / 10u) {
            return false;
        }
        magnitude = magnitude * 10u + digit;
        if (seen_point) {
            fraction_digits++;
        }
    }
    for (; fraction_digits < scale; fraction_digits++) {
        if (magnitude > UINT64_C(0x7FFFFFFFFFFFFFFF) / 10u) {
            return false;
        }
        magnitude *= 10u;
    }
    if (!seen_digit) {
        return false;
    }
    *value = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    return true;
}

/* Reads exactly `count` digits at *pos. */
static bool read_digits(field_ref field, size_t* pos, unsigned count, unsigned* value) {
    unsigned result = 0;
    for (unsigned i = 0; i < count; i++) {
        if (*pos >= field.length || field.text[*pos] < '0' || field.text[*pos] > '9') {
            return false;
        }
        result = result * 10u + (unsigned)(field.text[*pos] - '0');
        (*pos)++;
    }
    *value = result;
    return true;
}

static bool read_char(field_ref field, size_t* pos, char expected) {
    if (*pos >= field.length || field.text[*pos] != expected) {
        return false;
    }
    (*pos)++;
    return true;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm). */
static int64_t days_from_civil(int64_t year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    unsigned year_of_era = (unsigned)(year - era * 400);
    unsigned day_of_year = (153u * (month > 2 ? month - 3u : month + 9u) + 2u) / 5u + day - 1u;
    unsigned day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
    return era * 146097 + (int64_t)day_of_era - 719468;
}

/*
 * Parses "YYYY-MM-DD HH:MM:SS[.fraction]", "YYYYMMDD HH:MM:SS", or a date alone, with a
 * space or 'T' between date and time. The time is taken as UTC.
 */
static bool parse_timestamp(field_ref field, int64_t* value) {
    size_t pos = 0;
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!read_digits(field, &pos, 4, &year)) {
        return false;
    }
    bool dashed = pos < field.length && field.text[pos] == '-';
    if ((dashed && !read_char(field, &pos, '-')) || !read_digits(field, &pos, 2, &month) ||
        (dashed && !read_char(field, &pos, '-')) || !read_digits(field, &pos, 2, &day)) {
        return false;
    }

    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    int64_t nanos = 0;
    if (pos < field.length) {
        if (field.text[pos] != ' ' && field.text[pos] != 'T') {
            return false;
        }
        pos++;
        if (!read_digits(field, &pos, 2, &hour) || !read_char(field, &pos, ':') ||
            !read_digits(field, &pos, 2, &minute) || !read_char(field, &pos, ':') ||
            !read_digits(field, &pos, 2, &second)) {
            return false;
        }
        if (pos < field.length && field.text[pos] == '.') {
            pos++;
            int64_t scale = NANOS_PER_SECOND / 10;
            for (; pos < field.length && scale > 0; pos++, scale /= 10) {
                if (field.text[pos] < '0' || field.text[pos] > '9') {
                    return false;
                }
                nanos += (field.text[pos] - '0') * scale;
            }
        }
    }
    if (pos != field.length || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 60) {
        return false;
    }

    int64_t days = days_from_civil(year, month, day);
    int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second;
    *value = seconds * NANOS_PER_SECOND + nanos;
    return true;
}

/* ---------------------------------------------------------------------------------------- */
/* File list                                                                                */
/* ---------------------------------------------------------------------------------------- */

typedef struct file_list {
    char** paths;
    size_t count;
    size_t capacity;
} file_list;

static bool file_list_add(file_list* list, const char* path) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 64 : list->capacity * 2;
        char** grown = realloc(list->paths, capacity * sizeof(char*));
        if (grown == NULL) {
            return false;
        }
        list->paths = grown;
        list->capacity = capacity;
    }
    size_t length = strlen(path) + 1;
    list->paths[list->count] = malloc(length);
    if (list->paths[list->count] == NULL) {
        return false;
    }
    memcpy(list->paths[list->count], path, length);
    list->count++;
    return true;
}

static void file_list_free(file_list* list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->paths[i]);
    }
    free(list->paths);
}

static int compare_paths(const void* left, const void* right) {
    return strcmp(*(char* const*)left, *(char* const*)right);
}

/* Adds every regular, non-hidden file of `dir_path`, in name order. */
static bool add_directory(file_list* list, const char* dir_path, char* error, size_t error_size) {
    DIR* dir = opendir(dir_path);
    if (dir == NULL) {
        snprintf(error, error_size, "cannot open %s: %s", dir_path, strerror(errno));
        return false;
    }
    size_t first = list->count;
    bool ok = true;
    struct dirent* entry = NULL;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char path[4096];
        struct stat info;
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        if (stat(path, &info) == 0 && S_ISREG(info.st_mode)) {
            ok = file_list_add(list, path);
        }
    }
    closedir(dir);
    if (!ok) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    qsort(list->paths + first, list->count - first, sizeof(char*), compare_paths);
    return true;
}

static bool collect_files(char* const* paths, size_t path_count, file_list* list, char* error,
                          size_t error_size) {
    for (size_t i = 0; i < path_count; i++) {
        struct stat info;
        if (stat(paths[i], &info) != 0) {
            snprintf(error, error_size, "cannot open %s: %s", paths[i], strerror(errno));
            return false;
        }
        bool ok = S_ISDIR(info.st_mode) ? add_directory(list, paths[i], error, error_size)
                                        : file_list_add(list, paths[i]);
        if (!ok) {
            return false;
        }
    }
    if (list->count == 0) {
        snprintf(error, error_size, "no input files");
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------------------- */
/* Loading                                                                                  */
/* ---------------------------------------------------------------------------------------- */

/* Per-file state for one column: where the leading NULLs (if any) began. */
typedef struct column_file_state {
    bool has_value;        /* a non-NULL value has been seen in this file */
    uint32_t pending_from; /* first row of the leading NULL run, if !has_value */
    bool pending;          /* there are leading NULL rows waiting for a value */
} column_file_state;

/* Copies the value at row `from` into rows [first, last) of a fixed-width column. */
static void fill_rows(bench_column* column, uint32_t from, uint32_t first, uint32_t last) {
    for (uint32_t row = first; row < last; row++) {
        if (column->type == BENCH_TYPE_INT64) {
            column->ints[row] = column->ints[from];
        } else if (column->type == BENCH_TYPE_DOUBLE) {
            column->doubles[row] = column->doubles[from];
        } else if (column->type == BENCH_TYPE_BOOL) {
            column->bools[row] = column->bools[from];
        }
    }
}

/* Parses `field` into row `row` of `column`. Returns false if the text is invalid. */
static bool store_value(bench_column* column, const bench_column_spec* spec, uint32_t row,
                        field_ref field) {
    switch (spec->kind) {
        case BENCH_VALUE_INT64:
            return parse_int64(field, &column->ints[row]);
        case BENCH_VALUE_TIMESTAMP:
            return parse_timestamp(field, &column->ints[row]);
        case BENCH_VALUE_DECIMAL:
            return parse_decimal(field, spec->scale, &column->ints[row]);
        case BENCH_VALUE_DOUBLE:
            return parse_double(field, &column->doubles[row]);
        case BENCH_VALUE_BOOL:
            return parse_bool(field, &column->bools[row]);
        case BENCH_VALUE_STRING:
            return bench_column_set_string(column, row, field.text, field.length);
    }
    return false;
}

/* Stores one field, handling NULL (an empty field) as described in csv.h. */
static bool store_field(bench_column* column, const bench_column_spec* spec,
                        column_file_state* state, uint32_t row, field_ref field) {
    if (field.length > 0) {
        if (!store_value(column, spec, row, field)) {
            return false;
        }
        if (state->pending) {
            fill_rows(column, row, state->pending_from, row);
            state->pending = false;
        }
        state->has_value = true;
        return true;
    }

    if (!bench_column_mark_null(column, row)) {
        return false;
    }
    if (column->type == BENCH_TYPE_STRING) {
        return bench_column_set_string(column, row, "", 0);
    }
    if (state->has_value) {
        fill_rows(column, row - 1u, row, row + 1u);
    } else if (!state->pending) {
        state->pending = true;
        state->pending_from = row;
    }
    return true;
}

/* Tracks the chunk being built and closes it at series, partition, and size limits. */
typedef struct chunk_builder {
    uint32_t start;
    char split_value[MAX_SPLIT_VALUE];
    size_t split_length;
} chunk_builder;

static bool close_chunk(bench_csv_data* data, chunk_builder* builder, uint32_t end_row) {
    if (end_row == builder->start) {
        return true;
    }
    bool ok = bench_chunks_add(&data->chunks, builder->start, end_row - builder->start);
    builder->start = end_row;
    return ok;
}

static bool split_value_changed(chunk_builder* builder, field_ref field) {
    size_t length = field.length < MAX_SPLIT_VALUE ? field.length : MAX_SPLIT_VALUE;
    bool changed =
        length != builder->split_length || memcmp(builder->split_value, field.text, length) != 0;
    memcpy(builder->split_value, field.text, length);
    builder->split_length = length;
    return changed;
}

static bool reserve_rows(bench_csv_data* data, uint32_t rows) {
    for (size_t i = 0; i < data->column_count; i++) {
        bench_column* column = &data->columns[i];
        if (rows > column->capacity) {
            uint32_t capacity = column->capacity * 2u > rows ? column->capacity * 2u : rows;
            if (!bench_column_reserve(column, capacity)) {
                return false;
            }
        }
    }
    return true;
}

static bool load_file(const char* path, const bench_csv_options* options, bench_csv_data* data,
                      char* error, size_t error_size) {
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        snprintf(error, error_size, "cannot open %s: %s", path, strerror(errno));
        return false;
    }
    static char line[MAX_LINE];
    field_ref fields[MAX_FIELDS];
    column_file_state states[BENCH_MAX_CSV_COLUMNS] = {0};
    uint32_t row = data->columns[0].count;
    chunk_builder builder = {.start = row};
    uint32_t chunk_rows = options->chunk_rows != 0 ? options->chunk_rows : SX_VECTOR_SIZE;
    unsigned long line_number = 0;
    bool ok = true;

    while (ok && fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        if ((options->skip_header && line_number == 1) || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        size_t field_count = split_fields(line, fields, MAX_FIELDS);

        bool split_here = options->split && options->split_field < field_count &&
                          split_value_changed(&builder, fields[options->split_field]);
        if (row - builder.start == chunk_rows || (split_here && row > builder.start)) {
            ok = close_chunk(data, &builder, row);
        }
        if (ok && row == UINT32_MAX - 1u) {
            snprintf(error, error_size, "too many rows");
            ok = false;
        }
        ok = ok && reserve_rows(data, row + 1u);

        for (size_t c = 0; ok && c < options->column_count; c++) {
            const bench_column_spec* spec = &options->columns[c];
            if (spec->field >= field_count) {
                snprintf(error, error_size, "%s:%lu has no field %u", path, line_number,
                         spec->field);
                ok = false;
            } else if (!store_field(&data->columns[c], spec, &states[c], row,
                                    fields[spec->field])) {
                snprintf(error, error_size, "%s:%lu: cannot read \"%.*s\" as %s", path, line_number,
                         (int)fields[spec->field].length, fields[spec->field].text, spec->label);
                ok = false;
            }
        }
        if (ok) {
            row++;
            for (size_t c = 0; c < data->column_count; c++) {
                data->columns[c].count = row;
            }
        }
    }
    fclose(file);

    /* A column that was NULL for the whole file is filled with zeros (already zeroed). */
    ok = ok && close_chunk(data, &builder, row);
    return ok;
}

bool bench_load_csv(char* const* paths, size_t path_count, const bench_csv_options* options,
                    bench_csv_data* data, char* error, size_t error_size) {
    memset(data, 0, sizeof(*data));
    file_list files = {0};
    if (!collect_files(paths, path_count, &files, error, error_size)) {
        file_list_free(&files);
        return false;
    }

    data->column_count = options->column_count;
    bool ok = true;
    for (size_t c = 0; ok && c < options->column_count; c++) {
        ok = bench_column_init(&data->columns[c], storage_type(options->columns[c].kind), 1u << 20);
    }
    if (!ok) {
        snprintf(error, error_size, "out of memory");
    }

    for (size_t i = 0; ok && i < files.count; i++) {
        struct stat info;
        if (stat(files.paths[i], &info) == 0) {
            data->input_bytes += (uint64_t)info.st_size;
        }
        ok = load_file(files.paths[i], options, data, error, error_size);
        data->file_count++;
    }
    file_list_free(&files);

    if (ok && data->columns[0].count == 0) {
        snprintf(error, error_size, "the input has no data rows");
        ok = false;
    }
    if (!ok) {
        bench_csv_data_free(data);
    }
    return ok;
}

void bench_csv_data_free(bench_csv_data* data) {
    for (size_t c = 0; c < data->column_count; c++) {
        bench_column_free(&data->columns[c]);
    }
    bench_chunks_free(&data->chunks);
    memset(data, 0, sizeof(*data));
}
