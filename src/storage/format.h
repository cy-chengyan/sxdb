/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The FORMAT file of a data directory: its layout version, its random directory id, the
 * version that created it, and its enabled feature flags. See docs/DESIGN.md section 14.2.
 *
 * Plain ASCII text, one item per line, ending with the CRC32C of everything before the
 * last line:
 *
 *   sxdb-format 1
 *   directory_id 6f1c2a9e4b7d40e8a3c5f0b2d9e8a714
 *   created_by 0.1.0
 *   feature ro_compat example_feature 0.3.0
 *   crc32c 8b2f61d0
 *
 * Decoding is strict: any other line, a missing line, or a CRC mismatch is corruption.
 */

#ifndef SX_STORAGE_FORMAT_H
#define SX_STORAGE_FORMAT_H

#include "common/error.h"
#include "os/os_file.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest FORMAT file accepted. */
#define SX_FORMAT_FILE_MAX 4096u

#define SX_FORMAT_MAX_FEATURES 32u
#define SX_FORMAT_NAME_SIZE    64u /* feature names: [a-z0-9_], 1 to 63 characters */
#define SX_FORMAT_VERSION_SIZE 24u /* versions: [0-9A-Za-z.+-], 1 to 23 characters */

typedef enum sx_feature_class {
    SX_FEATURE_COMPAT,    /* an older binary ignores it */
    SX_FEATURE_RO_COMPAT, /* an older binary may only read */
    SX_FEATURE_INCOMPAT,  /* an older binary must refuse the directory */
} sx_feature_class;

typedef struct sx_format_feature {
    sx_feature_class feature_class;
    char name[SX_FORMAT_NAME_SIZE];
    char version[SX_FORMAT_VERSION_SIZE]; /* first version that understands it */
} sx_format_feature;

typedef struct sx_format {
    uint8_t directory_id[16];
    char created_by[SX_FORMAT_VERSION_SIZE];
    sx_format_feature features[SX_FORMAT_MAX_FEATURES];
    size_t feature_count;
} sx_format;

/* A new format: a random directory id, this version as creator, and no features. */
sx_err sx_format_init(sx_format* format);

/* Encodes the text of the FORMAT file. SX_ERR_LIMIT if `capacity` is too small. */
sx_err sx_format_encode(const sx_format* format, char* buffer, size_t capacity, size_t* size);

/* Decodes the text of a FORMAT file. SX_ERR_CORRUPT unless it is exactly well formed. */
sx_err sx_format_decode(const char* text, size_t size, sx_format* format);

/* Writes "FORMAT" in `dir` by atomic replacement. */
sx_err sx_format_write(sx_dir* dir, const sx_format* format);

/* Reads "FORMAT" from `dir`. SX_ERR_NOT_FOUND if there is none. */
sx_err sx_format_read(sx_dir* dir, sx_format* format);

/*
 * Checks that this binary understands every feature that matters for opening the
 * directory: unknown incompat features always block, unknown ro_compat features block
 * opening for writing. Returns SX_ERR_UNSUPPORTED and sets *blocking to the first such
 * feature, whose version says which sxdb is needed.
 */
sx_err sx_format_check(const sx_format* format, bool for_writing,
                       const sx_format_feature** blocking);

/* Writes the directory id as 32 lowercase hexadecimal digits and a terminating zero. */
void sx_format_directory_id_text(const sx_format* format, char text[33]);

#endif /* SX_STORAGE_FORMAT_H */
