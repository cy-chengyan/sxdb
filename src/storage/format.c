/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "storage/format.h"

#include "common/compiler.h"
#include "common/crc32c.h"
#include "os/os.h"

#include "sxdb.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define FIRST_LINE "sxdb-format 1"

/*
 * Features this binary understands. A new disk format feature adds its name here in the
 * release that introduces it; see docs/DESIGN.md section 14.2.
 */
static const char* const known_features[] = {
    NULL,
};

static const char* const class_names[] = {
    [SX_FEATURE_COMPAT] = "compat",
    [SX_FEATURE_RO_COMPAT] = "ro_compat",
    [SX_FEATURE_INCOMPAT] = "incompat",
};

/* ---------------------------------------------------------------------------------------- */
/* Tokens                                                                                   */
/* ---------------------------------------------------------------------------------------- */

static bool is_lower_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static bool is_version_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '+' || c == '-';
}

/* True if text[0, length) is non-empty, shorter than `size`, and passes `valid`. */
static bool is_token(const char* text, size_t length, size_t size, bool (*valid)(char)) {
    if (length == 0 || length >= size) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (!valid(text[i])) {
            return false;
        }
    }
    return true;
}

static bool is_name(const char* name) {
    return is_token(name, strnlen(name, SX_FORMAT_NAME_SIZE), SX_FORMAT_NAME_SIZE, is_name_char);
}

static bool is_version(const char* version) {
    return is_token(version, strnlen(version, SX_FORMAT_VERSION_SIZE), SX_FORMAT_VERSION_SIZE,
                    is_version_char);
}

static unsigned hex_value(char c) {
    return c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10);
}

/* ---------------------------------------------------------------------------------------- */
/* Encoding                                                                                 */
/* ---------------------------------------------------------------------------------------- */

void sx_format_directory_id_text(const sx_format* format, char text[33]) {
    for (size_t i = 0; i < sizeof(format->directory_id); i++) {
        snprintf(text + 2 * i, 3, "%02x", format->directory_id[i]);
    }
}

sx_err sx_format_init(sx_format* format) {
    memset(format, 0, sizeof(*format));
    snprintf(format->created_by, sizeof(format->created_by), "%s", SXDB_VERSION_STRING);
    return sx_os_random_bytes(format->directory_id, sizeof(format->directory_id));
}

/* Appends formatted text; returns false when it does not fit. */
static bool append(char* buffer, size_t capacity, size_t* position, const char* format, ...)
    SX_PRINTF_FORMAT(4, 5);

static bool append(char* buffer, size_t capacity, size_t* position, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *position, capacity - *position, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= capacity - *position) {
        return false;
    }
    *position += (size_t)written;
    return true;
}

sx_err sx_format_encode(const sx_format* format, char* buffer, size_t capacity, size_t* size) {
    if (!is_version(format->created_by) || format->feature_count > SX_FORMAT_MAX_FEATURES) {
        return SX_ERR_INVALID_ARG;
    }
    char id[33];
    sx_format_directory_id_text(format, id);
    size_t position = 0;
    bool fits = append(buffer, capacity, &position, FIRST_LINE "\n") &&
                append(buffer, capacity, &position, "directory_id %s\n", id) &&
                append(buffer, capacity, &position, "created_by %s\n", format->created_by);
    for (size_t i = 0; fits && i < format->feature_count; i++) {
        const sx_format_feature* feature = &format->features[i];
        if (!is_name(feature->name) || !is_version(feature->version) ||
            feature->feature_class > SX_FEATURE_INCOMPAT) {
            return SX_ERR_INVALID_ARG;
        }
        fits = append(buffer, capacity, &position, "feature %s %s %s\n",
                      class_names[feature->feature_class], feature->name, feature->version);
    }
    if (fits) {
        uint32_t crc = sx_crc32c(0, buffer, position);
        fits = append(buffer, capacity, &position, "crc32c %08x\n", (unsigned)crc);
    }
    if (!fits) {
        return SX_ERR_LIMIT;
    }
    *size = position;
    return SX_OK;
}

/* ---------------------------------------------------------------------------------------- */
/* Decoding                                                                                 */
/* ---------------------------------------------------------------------------------------- */

typedef struct line_reader {
    const char* text;
    size_t size;
    size_t position;
} line_reader;

/* Returns the next line without its LF; false if no complete line is left. */
static bool next_line(line_reader* reader, const char** line, size_t* length) {
    if (reader->position >= reader->size) {
        return false;
    }
    const char* start = reader->text + reader->position;
    const char* end = memchr(start, '\n', reader->size - reader->position);
    if (end == NULL) {
        return false;
    }
    *line = start;
    *length = (size_t)(end - start);
    reader->position += *length + 1;
    return true;
}

/* If the line is "<key> <rest>", returns true and points `rest` at the remainder. */
static bool split_key(const char* line, size_t length, const char* key, const char** rest,
                      size_t* rest_length) {
    size_t key_length = strlen(key);
    if (length <= key_length + 1 || memcmp(line, key, key_length) != 0 || line[key_length] != ' ') {
        return false;
    }
    *rest = line + key_length + 1;
    *rest_length = length - key_length - 1;
    return true;
}

/* Copies a validated token into a zero-terminated field. */
static bool take_token(const char* text, size_t length, char* out, size_t size,
                       bool (*valid)(char)) {
    if (!is_token(text, length, size, valid)) {
        return false;
    }
    memcpy(out, text, length);
    out[length] = '\0';
    return true;
}

static bool parse_feature(const char* text, size_t length, sx_format_feature* feature) {
    const char* first_space = memchr(text, ' ', length);
    if (first_space == NULL) {
        return false;
    }
    size_t class_length = (size_t)(first_space - text);
    const char* name = first_space + 1;
    size_t remaining = length - class_length - 1;
    const char* second_space = memchr(name, ' ', remaining);
    if (second_space == NULL) {
        return false;
    }
    size_t name_length = (size_t)(second_space - name);
    const char* version = second_space + 1;
    size_t version_length = remaining - name_length - 1;
    bool known_class = false;
    for (int c = SX_FEATURE_COMPAT; c <= SX_FEATURE_INCOMPAT; c++) {
        if (strlen(class_names[c]) == class_length &&
            memcmp(class_names[c], text, class_length) == 0) {
            feature->feature_class = (sx_feature_class)c;
            known_class = true;
        }
    }
    return known_class &&
           take_token(name, name_length, feature->name, SX_FORMAT_NAME_SIZE, is_name_char) &&
           take_token(version, version_length, feature->version, SX_FORMAT_VERSION_SIZE,
                      is_version_char);
}

static bool parse_crc(const char* text, size_t length, uint32_t* crc) {
    if (length != 8) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < 8; i++) {
        if (!is_lower_hex(text[i])) {
            return false;
        }
        value = value * 16 + hex_value(text[i]);
    }
    *crc = value;
    return true;
}

static bool decode_lines(line_reader* reader, sx_format* format) {
    const char* line;
    size_t length;
    const char* rest;
    size_t rest_length;

    if (!next_line(reader, &line, &length) || length != strlen(FIRST_LINE) ||
        memcmp(line, FIRST_LINE, length) != 0) {
        return false;
    }
    if (!next_line(reader, &line, &length) ||
        !split_key(line, length, "directory_id", &rest, &rest_length) || rest_length != 32) {
        return false;
    }
    for (size_t i = 0; i < 16; i++) {
        if (!is_lower_hex(rest[2 * i]) || !is_lower_hex(rest[2 * i + 1])) {
            return false;
        }
        format->directory_id[i] =
            (uint8_t)(hex_value(rest[2 * i]) * 16 + hex_value(rest[2 * i + 1]));
    }
    if (!next_line(reader, &line, &length) ||
        !split_key(line, length, "created_by", &rest, &rest_length) ||
        !take_token(rest, rest_length, format->created_by, SX_FORMAT_VERSION_SIZE,
                    is_version_char)) {
        return false;
    }
    for (;;) {
        size_t line_start = reader->position;
        if (!next_line(reader, &line, &length)) {
            return false;
        }
        if (split_key(line, length, "feature", &rest, &rest_length)) {
            if (format->feature_count == SX_FORMAT_MAX_FEATURES ||
                !parse_feature(rest, rest_length, &format->features[format->feature_count])) {
                return false;
            }
            format->feature_count++;
            continue;
        }
        uint32_t stored;
        if (!split_key(line, length, "crc32c", &rest, &rest_length) ||
            !parse_crc(rest, rest_length, &stored)) {
            return false;
        }
        /* The CRC covers everything before this line, and nothing may follow it. */
        return stored == sx_crc32c(0, reader->text, line_start) && reader->position == reader->size;
    }
}

sx_err sx_format_decode(const char* text, size_t size, sx_format* format) {
    memset(format, 0, sizeof(*format));
    line_reader reader = {text, size, 0};
    if (!decode_lines(&reader, format)) {
        memset(format, 0, sizeof(*format));
        return SX_ERR_CORRUPT;
    }
    return SX_OK;
}

/* ---------------------------------------------------------------------------------------- */
/* Files and checks                                                                         */
/* ---------------------------------------------------------------------------------------- */

sx_err sx_format_write(sx_dir* dir, const sx_format* format) {
    char text[SX_FORMAT_FILE_MAX];
    size_t size = 0;
    sx_err err = sx_format_encode(format, text, sizeof(text), &size);
    if (err != SX_OK) {
        return err;
    }
    return sx_dir_replace_file(dir, "FORMAT", text, size);
}

sx_err sx_format_read(sx_dir* dir, sx_format* format) {
    char text[SX_FORMAT_FILE_MAX];
    size_t size = 0;
    sx_err err = sx_dir_read_file(dir, "FORMAT", text, sizeof(text), &size);
    if (err == SX_ERR_LIMIT) {
        return SX_ERR_CORRUPT;
    }
    if (err != SX_OK) {
        return err;
    }
    return sx_format_decode(text, size, format);
}

static bool is_known_feature(const char* name) {
    for (size_t i = 0; known_features[i] != NULL; i++) {
        if (strcmp(known_features[i], name) == 0) {
            return true;
        }
    }
    return false;
}

sx_err sx_format_check(const sx_format* format, bool for_writing,
                       const sx_format_feature** blocking) {
    for (size_t i = 0; i < format->feature_count; i++) {
        const sx_format_feature* feature = &format->features[i];
        if (is_known_feature(feature->name)) {
            continue;
        }
        bool blocks = feature->feature_class == SX_FEATURE_INCOMPAT ||
                      (feature->feature_class == SX_FEATURE_RO_COMPAT && for_writing);
        if (blocks) {
            *blocking = feature;
            return SX_ERR_UNSUPPORTED;
        }
    }
    return SX_OK;
}
