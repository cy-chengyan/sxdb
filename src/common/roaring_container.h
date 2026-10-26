/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Containers of a Roaring bitmap: the values sharing their high 48 bits, kept as their low
 * 16 bits in one of three forms (docs/DESIGN.md section 5.5):
 *
 *   array   sorted u16 values, at most SX_RC_ARRAY_MAX of them
 *   bitmap  1024 u64 words, one bit per value
 *   run     sorted u16 pairs (start, length - 1), each covering start .. start + length - 1
 *
 * The data of all three has the byte layout of the portable serialization, so read-only
 * operations take a container either from a mutable bitmap or from serialized bytes,
 * through sx_rc_ref. Serialized data may be unaligned and may be corrupted: reads use
 * unaligned loads, runs reaching past 65535 are clipped, and iteration only moves forward,
 * so no input reads or writes outside the container or makes an iteration yield more than
 * 65536 values per container.
 *
 * Internal to the Roaring implementation (common/roaring.c).
 */

#ifndef SX_COMMON_ROARING_CONTAINER_H
#define SX_COMMON_ROARING_CONTAINER_H

#include "common/allocator.h"
#include "common/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SX_RC_ARRAY = 1,
    SX_RC_BITMAP = 2,
    SX_RC_RUN = 3,
};

#define SX_RC_ARRAY_MAX    4096u
#define SX_RC_BITMAP_WORDS 1024u
#define SX_RC_BITMAP_BYTES ((size_t)SX_RC_BITMAP_WORDS * 8u)
/* Beyond this many runs a bitmap is smaller in memory, so `add` converts. */
#define SX_RC_RUN_MAX 2048u

/* A container being read. */
typedef struct sx_rc_ref {
    uint8_t type;
    uint32_t cardinality;
    uint32_t count;      /* array: values; run: runs; bitmap: SX_RC_BITMAP_WORDS */
    const uint8_t* data; /* little-endian, possibly unaligned */
} sx_rc_ref;

/* A container of a mutable bitmap. Its data comes from the bitmap's allocator. */
typedef struct sx_rc {
    uint8_t type;
    uint32_t cardinality;
    uint32_t count;    /* array: values; run: runs; bitmap: SX_RC_BITMAP_WORDS */
    uint32_t capacity; /* array: values; run: runs; bitmap: SX_RC_BITMAP_WORDS */
    void* data;
} sx_rc;

/*
 * Position of an iteration within one container. Zero-initialize to start. `index` is the
 * next value (array), word (bitmap), or run; `word` holds the bits left of the current
 * bitmap word, or the smallest value a run may produce next.
 */
typedef struct sx_rc_cursor {
    uint32_t index;
    uint64_t word;
} sx_rc_cursor;

static inline sx_rc_ref sx_rc_as_ref(const sx_rc* container) {
    sx_rc_ref ref = {
        .type = container->type,
        .cardinality = container->cardinality,
        .count = container->count,
        .data = container->data,
    };
    return ref;
}

/* Bytes of data of a container of `type` with `count` values or runs. */
size_t sx_rc_data_size(uint8_t type, uint32_t count);

/* Releases the data of a container and leaves it empty. */
void sx_rc_release(sx_rc* container, const sx_allocator* allocator);

bool sx_rc_contains(const sx_rc_ref* container, uint16_t low);

/* Smallest and largest low 16 bits; the container must not be empty. */
uint16_t sx_rc_minimum(const sx_rc_ref* container);
uint16_t sx_rc_maximum(const sx_rc_ref* container);

/*
 * Writes up to `capacity` values, each `high` plus its low 16 bits, continuing from
 * `cursor`. Returns the number written; fewer than `capacity` means the container is done.
 */
size_t sx_rc_next(const sx_rc_ref* container, sx_rc_cursor* cursor, uint64_t high, uint64_t* values,
                  size_t capacity);

/* Starts a container holding the single value `low`. */
sx_err sx_rc_init_single(sx_rc* container, uint16_t low, const sx_allocator* allocator);

/* Adds `low`; sets *added if it was not present yet. */
sx_err sx_rc_add(sx_rc* container, uint16_t low, bool* added, const sx_allocator* allocator);

/* Adds every value from `first` to `last` (inclusive) to a container, which may be empty. */
sx_err sx_rc_add_range(sx_rc* container, uint16_t first, uint16_t last,
                       const sx_allocator* allocator);

/* Copies a container. */
sx_err sx_rc_copy(sx_rc* out, const sx_rc_ref* in, const sx_allocator* allocator);

/* Set operations. The result may be empty (cardinality 0), with no data allocated. */
sx_err sx_rc_and(sx_rc* out, const sx_rc_ref* a, const sx_rc_ref* b, const sx_allocator* allocator);
sx_err sx_rc_or(sx_rc* out, const sx_rc_ref* a, const sx_rc_ref* b, const sx_allocator* allocator);
sx_err sx_rc_andnot(sx_rc* out, const sx_rc_ref* a, const sx_rc_ref* b,
                    const sx_allocator* allocator);

/*
 * Serialization. A container is written as a run container when that is smaller, and
 * otherwise as an array (up to SX_RC_ARRAY_MAX values) or a bitmap, as the portable
 * format requires.
 */
bool sx_rc_serialize_as_run(const sx_rc_ref* container);
size_t sx_rc_serialized_size(const sx_rc_ref* container, bool as_run);
void sx_rc_serialize(const sx_rc_ref* container, bool as_run, uint8_t* out);

#endif /* SX_COMMON_ROARING_CONTAINER_H */
