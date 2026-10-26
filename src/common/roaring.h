/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * 64-bit Roaring bitmaps. See docs/DESIGN.md section 5.5.
 *
 * Values are split into containers by their high 48 bits; each container holds the low
 * 16 bits as a sorted array, a bitmap, or runs (common/roaring_container.h).
 *
 * Two forms:
 *   - sx_roaring: a mutable bitmap whose memory comes from an allocator chosen by its
 *     owner (common/allocator.h);
 *   - sx_roaring_view: a serialized bitmap used in place, for example in a buffer pool
 *     frame. Opening a view checks its structure in time proportional to its number of
 *     containers; after that every operation stays within its bytes and takes bounded
 *     time whatever they hold (a damaged view gives wrong results, nothing worse).
 *
 * Read-only operations take either form through sx_roaring_ref. Operations that build a
 * bitmap write into a separate result.
 *
 * Serialized format: the portable format of the Roaring format specification with its
 * 64-bit extension (a u64 count of 32-bit bitmaps, each preceded by its high 32 bits as a
 * u32), so other Roaring implementations read what this one writes. Offset headers, when
 * present, must match the container sizes exactly; this implementation writes them so.
 */

#ifndef SX_COMMON_ROARING_H
#define SX_COMMON_ROARING_H

#include "common/allocator.h"
#include "common/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sx_rc;

typedef struct sx_roaring {
    sx_allocator allocator;
    uint64_t* keys;           /* high 48 bits of each container's values, ascending */
    struct sx_rc* containers; /* parallel to keys; none is empty */
    size_t count;
    size_t capacity;
} sx_roaring;

typedef struct sx_roaring_view {
    const uint8_t* data;
    size_t size;
    uint64_t cardinality;
    uint64_t container_count;
} sx_roaring_view;

/* An operand of the read-only operations: exactly one of the two pointers is set. */
typedef struct sx_roaring_ref {
    const sx_roaring* bitmap;
    const sx_roaring_view* view;
} sx_roaring_ref;

static inline sx_roaring_ref sx_roaring_ref_bitmap(const sx_roaring* bitmap) {
    sx_roaring_ref ref = {bitmap, NULL};
    return ref;
}

static inline sx_roaring_ref sx_roaring_ref_view(const sx_roaring_view* view) {
    sx_roaring_ref ref = {NULL, view};
    return ref;
}

/* Walks the containers of a bitmap in key order. Internal to the implementation. */
typedef struct sx_roaring_walker {
    const sx_roaring* bitmap;
    size_t index;
    const uint8_t* data;
    uint64_t buckets_left;
    uint64_t high;
    uint32_t bucket_count;
    uint32_t bucket_index;
    bool has_runs;
    size_t run_flags;
    size_t headers;
    size_t position;
} sx_roaring_walker;

/* Iterates the values of a bitmap in ascending order, in batches. */
typedef struct sx_roaring_iterator {
    sx_roaring_walker walker;
    bool in_container;
    uint64_t high;
    const uint8_t* container_data;
    uint8_t container_type;
    uint32_t container_cardinality;
    uint32_t container_count;
    uint32_t cursor_index;
    uint64_t cursor_word;
} sx_roaring_iterator;

/* Starts an empty bitmap that allocates from `allocator` (copied). */
void sx_roaring_init(sx_roaring* bitmap, const sx_allocator* allocator);

/* Releases all memory; the bitmap is empty afterwards and can be used again. */
void sx_roaring_clear(sx_roaring* bitmap);

sx_err sx_roaring_add(sx_roaring* bitmap, uint64_t value);

/* Adds every value in [start, end). */
sx_err sx_roaring_add_range(sx_roaring* bitmap, uint64_t start, uint64_t end);

bool sx_roaring_contains(sx_roaring_ref bitmap, uint64_t value);
uint64_t sx_roaring_cardinality(sx_roaring_ref bitmap);
bool sx_roaring_is_empty(sx_roaring_ref bitmap);

/* Smallest and largest value; false if the bitmap is empty. */
bool sx_roaring_minimum(sx_roaring_ref bitmap, uint64_t* value);
bool sx_roaring_maximum(sx_roaring_ref bitmap, uint64_t* value);

/*
 * Set operations. `result` is cleared first and must not be one of the operands; on
 * failure it is left empty.
 */
sx_err sx_roaring_and(sx_roaring* result, sx_roaring_ref a, sx_roaring_ref b);
sx_err sx_roaring_or(sx_roaring* result, sx_roaring_ref a, sx_roaring_ref b);
sx_err sx_roaring_andnot(sx_roaring* result, sx_roaring_ref a, sx_roaring_ref b);
sx_err sx_roaring_copy(sx_roaring* result, sx_roaring_ref source);

/* Adds every value of `source` to `target`; for the union of many bitmaps. */
sx_err sx_roaring_or_into(sx_roaring* target, sx_roaring_ref source);

void sx_roaring_iterator_init(sx_roaring_iterator* iterator, sx_roaring_ref bitmap);

/* Writes up to `capacity` next values; returns how many (0 at the end). */
size_t sx_roaring_iterator_next(sx_roaring_iterator* iterator, uint64_t* values, size_t capacity);

size_t sx_roaring_serialized_size(const sx_roaring* bitmap);

/* Serializes into `buffer`; SX_ERR_LIMIT if `capacity` is too small. */
sx_err sx_roaring_serialize(const sx_roaring* bitmap, void* buffer, size_t capacity, size_t* size);

/*
 * Opens `size` bytes holding exactly one serialized bitmap. SX_ERR_CORRUPT if the
 * structure is invalid. The view refers to `data`, which must outlive it.
 */
sx_err sx_roaring_view_open(sx_roaring_view* view, const void* data, size_t size);

#endif /* SX_COMMON_ROARING_H */
