/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "common/roaring.h"

#include "common/bits.h"
#include "common/roaring_container.h"

#include <string.h>

/* Cookies of the portable 32-bit format. */
#define COOKIE_NO_RUNS        12346u
#define COOKIE_WITH_RUNS      12347u
#define NO_OFFSET_THRESHOLD   4u
#define MAX_BUCKET_CONTAINERS 65536u

/* ---------------------------------------------------------------------------------------- */
/* Mutable bitmaps                                                                          */
/* ---------------------------------------------------------------------------------------- */

void sx_roaring_init(sx_roaring* bitmap, const sx_allocator* allocator) {
    memset(bitmap, 0, sizeof(*bitmap));
    bitmap->allocator = *allocator;
}

void sx_roaring_clear(sx_roaring* bitmap) {
    const sx_allocator* allocator = &bitmap->allocator;
    for (size_t i = 0; i < bitmap->count; i++) {
        sx_rc_release(&bitmap->containers[i], allocator);
    }
    sx_release(allocator, bitmap->keys, bitmap->capacity * sizeof(uint64_t));
    sx_release(allocator, bitmap->containers, bitmap->capacity * sizeof(sx_rc));
    bitmap->keys = NULL;
    bitmap->containers = NULL;
    bitmap->count = 0;
    bitmap->capacity = 0;
}

static sx_err reserve_containers(sx_roaring* bitmap, size_t needed) {
    if (needed <= bitmap->capacity) {
        return SX_OK;
    }
    size_t capacity = bitmap->capacity < 4 ? 4 : bitmap->capacity * 2;
    if (capacity < needed) {
        capacity = needed;
    }
    const sx_allocator* allocator = &bitmap->allocator;
    uint64_t* keys = sx_allocate(allocator, capacity * sizeof(uint64_t));
    sx_rc* containers = sx_allocate(allocator, capacity * sizeof(sx_rc));
    if (keys == NULL || containers == NULL) {
        sx_release(allocator, keys, capacity * sizeof(uint64_t));
        sx_release(allocator, containers, capacity * sizeof(sx_rc));
        return SX_ERR_NOMEM;
    }
    if (bitmap->count > 0) {
        memcpy(keys, bitmap->keys, bitmap->count * sizeof(uint64_t));
        memcpy(containers, bitmap->containers, bitmap->count * sizeof(sx_rc));
    }
    sx_release(allocator, bitmap->keys, bitmap->capacity * sizeof(uint64_t));
    sx_release(allocator, bitmap->containers, bitmap->capacity * sizeof(sx_rc));
    bitmap->keys = keys;
    bitmap->containers = containers;
    bitmap->capacity = capacity;
    return SX_OK;
}

/* Index of the first container whose key is >= `key`. */
static size_t lower_bound(const sx_roaring* bitmap, uint64_t key) {
    if (bitmap->count > 0 && bitmap->keys[bitmap->count - 1] < key) {
        return bitmap->count; /* appending in order is the common case */
    }
    size_t lo = 0;
    size_t hi = bitmap->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (bitmap->keys[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* Inserts a container at `index`; the bitmap takes over its data. */
static sx_err insert_container(sx_roaring* bitmap, size_t index, uint64_t key,
                               const sx_rc* container) {
    sx_err err = reserve_containers(bitmap, bitmap->count + 1);
    if (err != SX_OK) {
        return err;
    }
    size_t tail = bitmap->count - index;
    memmove(&bitmap->keys[index + 1], &bitmap->keys[index], tail * sizeof(uint64_t));
    memmove(&bitmap->containers[index + 1], &bitmap->containers[index], tail * sizeof(sx_rc));
    bitmap->keys[index] = key;
    bitmap->containers[index] = *container;
    bitmap->count++;
    return SX_OK;
}

/* Appends a container (keys must arrive in ascending order); empty ones are dropped. */
static sx_err append_container(sx_roaring* bitmap, uint64_t key, sx_rc* container) {
    if (container->cardinality == 0) {
        sx_rc_release(container, &bitmap->allocator);
        return SX_OK;
    }
    sx_err err = insert_container(bitmap, bitmap->count, key, container);
    if (err != SX_OK) {
        sx_rc_release(container, &bitmap->allocator);
    }
    return err;
}

sx_err sx_roaring_add(sx_roaring* bitmap, uint64_t value) {
    uint64_t key = value >> 16;
    uint16_t low = (uint16_t)value;
    size_t index = lower_bound(bitmap, key);
    if (index < bitmap->count && bitmap->keys[index] == key) {
        bool added;
        return sx_rc_add(&bitmap->containers[index], low, &added, &bitmap->allocator);
    }
    sx_rc container;
    sx_err err = sx_rc_init_single(&container, low, &bitmap->allocator);
    if (err != SX_OK) {
        return err;
    }
    err = insert_container(bitmap, index, key, &container);
    if (err != SX_OK) {
        sx_rc_release(&container, &bitmap->allocator);
    }
    return err;
}

sx_err sx_roaring_add_range(sx_roaring* bitmap, uint64_t start, uint64_t end) {
    if (start >= end) {
        return SX_OK;
    }
    uint64_t last_value = end - 1;
    for (uint64_t key = start >> 16; key <= last_value >> 16; key++) {
        uint16_t first = key == start >> 16 ? (uint16_t)start : 0;
        uint16_t last = key == last_value >> 16 ? (uint16_t)last_value : UINT16_MAX;
        size_t index = lower_bound(bitmap, key);
        sx_err err;
        if (index < bitmap->count && bitmap->keys[index] == key) {
            err = sx_rc_add_range(&bitmap->containers[index], first, last, &bitmap->allocator);
        } else {
            sx_rc container;
            memset(&container, 0, sizeof(container));
            err = sx_rc_add_range(&container, first, last, &bitmap->allocator);
            if (err == SX_OK) {
                err = insert_container(bitmap, index, key, &container);
                if (err != SX_OK) {
                    sx_rc_release(&container, &bitmap->allocator);
                }
            }
        }
        if (err != SX_OK) {
            return err;
        }
    }
    return SX_OK;
}

/* ---------------------------------------------------------------------------------------- */
/* Serialized bitmaps: one 32-bit bucket at a time                                          */
/* ---------------------------------------------------------------------------------------- */

/* Parses the bucket header at walker->position (already validated). */
static void begin_bucket(sx_roaring_walker* walker) {
    const uint8_t* data = walker->data;
    size_t position = walker->position;
    walker->high = (uint64_t)sx_load_u32(data + position) << 32;
    position += 4;
    uint32_t cookie = sx_load_u32(data + position);
    position += 4;
    uint32_t count;
    if ((cookie & 0xFFFFu) == COOKIE_WITH_RUNS) {
        count = (cookie >> 16) + 1;
        walker->has_runs = true;
        walker->run_flags = position;
        position += (count + 7) / 8;
    } else {
        count = sx_load_u32(data + position);
        position += 4;
        walker->has_runs = false;
    }
    walker->headers = position;
    position += 4 * (size_t)count;
    if (!walker->has_runs || count >= NO_OFFSET_THRESHOLD) {
        position += 4 * (size_t)count;
    }
    walker->position = position;
    walker->bucket_count = count;
    walker->bucket_index = 0;
    walker->buckets_left--;
}

/* Describes container `index` of the current bucket, whose data starts at `position`. */
static sx_rc_ref bucket_container(const sx_roaring_walker* walker, uint32_t index, size_t position,
                                  uint16_t* key, size_t* size) {
    const uint8_t* data = walker->data;
    const uint8_t* header = data + walker->headers + 4 * (size_t)index;
    *key = sx_load_u16(header);
    uint32_t cardinality = (uint32_t)sx_load_u16(header + 2) + 1;
    bool is_run =
        walker->has_runs && ((data[walker->run_flags + index / 8] >> (index % 8)) & 1) != 0;
    sx_rc_ref ref;
    ref.cardinality = cardinality;
    if (is_run) {
        ref.type = SX_RC_RUN;
        ref.count = sx_load_u16(data + position);
        ref.data = data + position + 2;
        *size = 2 + 4 * (size_t)ref.count;
    } else if (cardinality <= SX_RC_ARRAY_MAX) {
        ref.type = SX_RC_ARRAY;
        ref.count = cardinality;
        ref.data = data + position;
        *size = 2 * (size_t)cardinality;
    } else {
        ref.type = SX_RC_BITMAP;
        ref.count = SX_RC_BITMAP_WORDS;
        ref.data = data + position;
        *size = SX_RC_BITMAP_BYTES;
    }
    return ref;
}

/* ---------------------------------------------------------------------------------------- */
/* Walking containers of either form                                                        */
/* ---------------------------------------------------------------------------------------- */

static void walker_init(sx_roaring_walker* walker, sx_roaring_ref ref) {
    memset(walker, 0, sizeof(*walker));
    if (ref.bitmap != NULL) {
        walker->bitmap = ref.bitmap;
        return;
    }
    walker->data = ref.view->data;
    walker->buckets_left = sx_load_u64(ref.view->data);
    walker->position = 8;
}

/* Produces the next container and its 48-bit key; false at the end. */
static bool walker_next(sx_roaring_walker* walker, uint64_t* key, sx_rc_ref* container) {
    if (walker->bitmap != NULL) {
        if (walker->index >= walker->bitmap->count) {
            return false;
        }
        *key = walker->bitmap->keys[walker->index];
        *container = sx_rc_as_ref(&walker->bitmap->containers[walker->index]);
        walker->index++;
        return true;
    }
    while (walker->bucket_index == walker->bucket_count) {
        if (walker->buckets_left == 0) {
            return false;
        }
        begin_bucket(walker);
    }
    uint16_t low_key;
    size_t size;
    *container = bucket_container(walker, walker->bucket_index, walker->position, &low_key, &size);
    *key = (walker->high >> 16) | low_key;
    walker->position += size;
    walker->bucket_index++;
    return true;
}

/* ---------------------------------------------------------------------------------------- */
/* Opening a view                                                                           */
/* ---------------------------------------------------------------------------------------- */

/* Bounds-checked parse state for validation. */
typedef struct validator {
    const uint8_t* data;
    size_t size;
    size_t position;
} validator;

static bool take(validator* v, size_t count) {
    if (count > v->size - v->position) {
        return false;
    }
    v->position += count;
    return true;
}

/* Validates one 32-bit bucket starting at v->position. */
static bool validate_bucket(validator* v, uint64_t* cardinality, uint64_t* containers) {
    size_t start = v->position;
    if (!take(v, 4)) {
        return false;
    }
    uint32_t cookie = sx_load_u32(v->data + start);
    uint32_t count;
    bool has_runs;
    size_t run_flags = 0;
    if ((cookie & 0xFFFFu) == COOKIE_WITH_RUNS) {
        count = (cookie >> 16) + 1;
        has_runs = true;
        run_flags = v->position;
        if (!take(v, (count + 7) / 8)) {
            return false;
        }
    } else if (cookie == COOKIE_NO_RUNS) {
        size_t at = v->position;
        if (!take(v, 4)) {
            return false;
        }
        count = sx_load_u32(v->data + at);
        has_runs = false;
        if (count > MAX_BUCKET_CONTAINERS) {
            return false;
        }
    } else {
        return false;
    }
    size_t headers = v->position;
    if (!take(v, 4 * (size_t)count)) {
        return false;
    }
    size_t offsets = 0;
    bool has_offsets = !has_runs || count >= NO_OFFSET_THRESHOLD;
    if (has_offsets) {
        offsets = v->position;
        if (!take(v, 4 * (size_t)count)) {
            return false;
        }
    }
    uint32_t previous_key = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* header = v->data + headers + 4 * (size_t)i;
        uint32_t key = sx_load_u16(header);
        uint32_t container_cardinality = (uint32_t)sx_load_u16(header + 2) + 1;
        if (i > 0 && key <= previous_key) {
            return false;
        }
        previous_key = key;
        if (has_offsets && sx_load_u32(v->data + offsets + 4 * (size_t)i) != v->position - start) {
            return false;
        }
        bool is_run = has_runs && ((v->data[run_flags + i / 8] >> (i % 8)) & 1) != 0;
        size_t size;
        if (is_run) {
            size_t at = v->position;
            if (!take(v, 2)) {
                return false;
            }
            size = 4 * (size_t)sx_load_u16(v->data + at);
        } else if (container_cardinality <= SX_RC_ARRAY_MAX) {
            size = 2 * (size_t)container_cardinality;
        } else {
            size = SX_RC_BITMAP_BYTES;
        }
        if (!take(v, size)) {
            return false;
        }
        *cardinality += container_cardinality;
    }
    *containers += count;
    return true;
}

sx_err sx_roaring_view_open(sx_roaring_view* view, const void* data, size_t size) {
    validator v = {data, size, 0};
    if (!take(&v, 8)) {
        return SX_ERR_CORRUPT;
    }
    uint64_t buckets = sx_load_u64(v.data);
    uint64_t cardinality = 0;
    uint64_t containers = 0;
    uint64_t previous_high = 0;
    for (uint64_t i = 0; i < buckets; i++) {
        size_t at = v.position;
        if (!take(&v, 4)) {
            return SX_ERR_CORRUPT;
        }
        uint64_t high = sx_load_u32(v.data + at);
        if (i > 0 && high <= previous_high) {
            return SX_ERR_CORRUPT;
        }
        previous_high = high;
        if (!validate_bucket(&v, &cardinality, &containers)) {
            return SX_ERR_CORRUPT;
        }
    }
    if (v.position != size) {
        return SX_ERR_CORRUPT;
    }
    view->data = data;
    view->size = size;
    view->cardinality = cardinality;
    view->container_count = containers;
    return SX_OK;
}

/* ---------------------------------------------------------------------------------------- */
/* Queries                                                                                  */
/* ---------------------------------------------------------------------------------------- */

static bool find_container(sx_roaring_ref ref, uint64_t key, sx_rc_ref* found) {
    if (ref.bitmap != NULL) {
        size_t index = lower_bound(ref.bitmap, key);
        if (index == ref.bitmap->count || ref.bitmap->keys[index] != key) {
            return false;
        }
        *found = sx_rc_as_ref(&ref.bitmap->containers[index]);
        return true;
    }
    sx_roaring_walker walker;
    walker_init(&walker, ref);
    uint64_t container_key;
    sx_rc_ref container;
    while (walker_next(&walker, &container_key, &container)) {
        if (container_key == key) {
            *found = container;
            return true;
        }
        if (container_key > key) {
            return false;
        }
    }
    return false;
}

bool sx_roaring_contains(sx_roaring_ref bitmap, uint64_t value) {
    sx_rc_ref container;
    if (!find_container(bitmap, value >> 16, &container)) {
        return false;
    }
    return sx_rc_contains(&container, (uint16_t)value);
}

uint64_t sx_roaring_cardinality(sx_roaring_ref bitmap) {
    if (bitmap.view != NULL) {
        return bitmap.view->cardinality;
    }
    uint64_t total = 0;
    for (size_t i = 0; i < bitmap.bitmap->count; i++) {
        total += bitmap.bitmap->containers[i].cardinality;
    }
    return total;
}

bool sx_roaring_is_empty(sx_roaring_ref bitmap) {
    if (bitmap.view != NULL) {
        return bitmap.view->container_count == 0;
    }
    return bitmap.bitmap->count == 0;
}

bool sx_roaring_minimum(sx_roaring_ref bitmap, uint64_t* value) {
    sx_roaring_walker walker;
    walker_init(&walker, bitmap);
    uint64_t key;
    sx_rc_ref container;
    if (!walker_next(&walker, &key, &container)) {
        return false;
    }
    *value = (key << 16) | sx_rc_minimum(&container);
    return true;
}

bool sx_roaring_maximum(sx_roaring_ref bitmap, uint64_t* value) {
    if (bitmap.bitmap != NULL) {
        const sx_roaring* mutable_bitmap = bitmap.bitmap;
        if (mutable_bitmap->count == 0) {
            return false;
        }
        sx_rc_ref last = sx_rc_as_ref(&mutable_bitmap->containers[mutable_bitmap->count - 1]);
        *value = (mutable_bitmap->keys[mutable_bitmap->count - 1] << 16) | sx_rc_maximum(&last);
        return true;
    }
    sx_roaring_walker walker;
    walker_init(&walker, bitmap);
    uint64_t key;
    sx_rc_ref container;
    bool found = false;
    uint64_t last_key = 0;
    sx_rc_ref last_container;
    while (walker_next(&walker, &key, &container)) {
        last_key = key;
        last_container = container;
        found = true;
    }
    if (found) {
        *value = (last_key << 16) | sx_rc_maximum(&last_container);
    }
    return found;
}

/* ---------------------------------------------------------------------------------------- */
/* Set operations                                                                           */
/* ---------------------------------------------------------------------------------------- */

typedef enum set_operation {
    OPERATION_AND,
    OPERATION_OR,
    OPERATION_ANDNOT,
} set_operation;

static bool refers_to(sx_roaring_ref ref, const sx_roaring* bitmap) {
    return ref.bitmap == bitmap;
}

/* Merges the containers of `a` and `b` in key order into `result`. */
static sx_err combine(sx_roaring* result, sx_roaring_ref a, sx_roaring_ref b,
                      set_operation operation) {
    if (refers_to(a, result) || refers_to(b, result)) {
        return SX_ERR_INVALID_ARG;
    }
    sx_roaring_clear(result);
    const sx_allocator* allocator = &result->allocator;
    sx_roaring_walker walker_a;
    sx_roaring_walker walker_b;
    walker_init(&walker_a, a);
    walker_init(&walker_b, b);
    uint64_t key_a = 0;
    uint64_t key_b = 0;
    sx_rc_ref container_a;
    sx_rc_ref container_b;
    bool has_a = walker_next(&walker_a, &key_a, &container_a);
    bool has_b = walker_next(&walker_b, &key_b, &container_b);
    sx_err err = SX_OK;
    while (err == SX_OK && (has_a || has_b)) {
        sx_rc out;
        memset(&out, 0, sizeof(out));
        if (has_a && (!has_b || key_a < key_b)) {
            if (operation != OPERATION_AND) {
                err = sx_rc_copy(&out, &container_a, allocator);
                if (err == SX_OK) {
                    err = append_container(result, key_a, &out);
                }
            }
            has_a = walker_next(&walker_a, &key_a, &container_a);
        } else if (has_b && (!has_a || key_b < key_a)) {
            if (operation == OPERATION_OR) {
                err = sx_rc_copy(&out, &container_b, allocator);
                if (err == SX_OK) {
                    err = append_container(result, key_b, &out);
                }
            }
            has_b = walker_next(&walker_b, &key_b, &container_b);
        } else {
            if (operation == OPERATION_AND) {
                err = sx_rc_and(&out, &container_a, &container_b, allocator);
            } else if (operation == OPERATION_OR) {
                err = sx_rc_or(&out, &container_a, &container_b, allocator);
            } else {
                err = sx_rc_andnot(&out, &container_a, &container_b, allocator);
            }
            if (err == SX_OK) {
                err = append_container(result, key_a, &out);
            }
            has_a = walker_next(&walker_a, &key_a, &container_a);
            has_b = walker_next(&walker_b, &key_b, &container_b);
        }
    }
    if (err != SX_OK) {
        sx_roaring_clear(result);
    }
    return err;
}

sx_err sx_roaring_and(sx_roaring* result, sx_roaring_ref a, sx_roaring_ref b) {
    return combine(result, a, b, OPERATION_AND);
}

sx_err sx_roaring_or(sx_roaring* result, sx_roaring_ref a, sx_roaring_ref b) {
    return combine(result, a, b, OPERATION_OR);
}

sx_err sx_roaring_andnot(sx_roaring* result, sx_roaring_ref a, sx_roaring_ref b) {
    return combine(result, a, b, OPERATION_ANDNOT);
}

sx_err sx_roaring_copy(sx_roaring* result, sx_roaring_ref source) {
    sx_roaring empty;
    sx_roaring_init(&empty, &result->allocator);
    return combine(result, source, sx_roaring_ref_bitmap(&empty), OPERATION_OR);
}

sx_err sx_roaring_or_into(sx_roaring* target, sx_roaring_ref source) {
    if (refers_to(source, target)) {
        return SX_OK;
    }
    sx_roaring merged;
    sx_roaring_init(&merged, &target->allocator);
    sx_err err = combine(&merged, sx_roaring_ref_bitmap(target), source, OPERATION_OR);
    if (err != SX_OK) {
        return err;
    }
    sx_roaring_clear(target);
    *target = merged;
    return SX_OK;
}

/* ---------------------------------------------------------------------------------------- */
/* Iteration                                                                                */
/* ---------------------------------------------------------------------------------------- */

void sx_roaring_iterator_init(sx_roaring_iterator* iterator, sx_roaring_ref bitmap) {
    memset(iterator, 0, sizeof(*iterator));
    walker_init(&iterator->walker, bitmap);
}

size_t sx_roaring_iterator_next(sx_roaring_iterator* iterator, uint64_t* values, size_t capacity) {
    size_t written = 0;
    while (written < capacity) {
        if (!iterator->in_container) {
            uint64_t key;
            sx_rc_ref container;
            if (!walker_next(&iterator->walker, &key, &container)) {
                break;
            }
            iterator->in_container = true;
            iterator->high = key << 16;
            iterator->container_data = container.data;
            iterator->container_type = container.type;
            iterator->container_cardinality = container.cardinality;
            iterator->container_count = container.count;
            iterator->cursor_index = 0;
            iterator->cursor_word = 0;
        }
        sx_rc_ref container = {
            .type = iterator->container_type,
            .cardinality = iterator->container_cardinality,
            .count = iterator->container_count,
            .data = iterator->container_data,
        };
        sx_rc_cursor cursor = {
            .index = iterator->cursor_index,
            .word = iterator->cursor_word,
        };
        size_t wanted = capacity - written;
        size_t got = sx_rc_next(&container, &cursor, iterator->high, values + written, wanted);
        iterator->cursor_index = cursor.index;
        iterator->cursor_word = cursor.word;
        written += got;
        if (got < wanted) {
            iterator->in_container = false;
        }
    }
    return written;
}

/* ---------------------------------------------------------------------------------------- */
/* Serialization                                                                            */
/* ---------------------------------------------------------------------------------------- */

/* Containers [first, end) share their high 32 bits. Returns end. */
static size_t bucket_end(const sx_roaring* bitmap, size_t first) {
    uint64_t high = bitmap->keys[first] >> 16;
    size_t end = first + 1;
    while (end < bitmap->count && bitmap->keys[end] >> 16 == high) {
        end++;
    }
    return end;
}

/* Size of the bucket header, headers, and offsets for `count` containers. */
static size_t bucket_header_size(size_t count, bool has_runs) {
    size_t size = has_runs ? 4 + (count + 7) / 8 : 8;
    size += 4 * count;
    if (!has_runs || count >= NO_OFFSET_THRESHOLD) {
        size += 4 * count;
    }
    return size;
}

static size_t bucket_size(const sx_roaring* bitmap, size_t first, size_t end) {
    bool has_runs = false;
    size_t data = 0;
    for (size_t i = first; i < end; i++) {
        sx_rc_ref ref = sx_rc_as_ref(&bitmap->containers[i]);
        bool as_run = sx_rc_serialize_as_run(&ref);
        has_runs = has_runs || as_run;
        data += sx_rc_serialized_size(&ref, as_run);
    }
    return bucket_header_size(end - first, has_runs) + data;
}

size_t sx_roaring_serialized_size(const sx_roaring* bitmap) {
    size_t size = 8;
    for (size_t first = 0; first < bitmap->count;) {
        size_t end = bucket_end(bitmap, first);
        size += 4 + bucket_size(bitmap, first, end);
        first = end;
    }
    return size;
}

/* Writes one bucket at `out`; returns the bytes written. */
static size_t serialize_bucket(const sx_roaring* bitmap, size_t first, size_t end, uint8_t* out) {
    size_t count = end - first;
    bool has_runs = false;
    for (size_t i = first; i < end; i++) {
        sx_rc_ref ref = sx_rc_as_ref(&bitmap->containers[i]);
        has_runs = has_runs || sx_rc_serialize_as_run(&ref);
    }
    size_t position = 0;
    size_t run_flags = 0;
    if (has_runs) {
        sx_store_u32(out, COOKIE_WITH_RUNS | ((uint32_t)(count - 1) << 16));
        position = 4;
        run_flags = position;
        memset(out + position, 0, (count + 7) / 8);
        position += (count + 7) / 8;
    } else {
        sx_store_u32(out, COOKIE_NO_RUNS);
        sx_store_u32(out + 4, (uint32_t)count);
        position = 8;
    }
    size_t headers = position;
    position += 4 * count;
    bool has_offsets = !has_runs || count >= NO_OFFSET_THRESHOLD;
    size_t offsets = position;
    if (has_offsets) {
        position += 4 * count;
    }
    for (size_t i = 0; i < count; i++) {
        const sx_rc* container = &bitmap->containers[first + i];
        sx_rc_ref ref = sx_rc_as_ref(container);
        bool as_run = sx_rc_serialize_as_run(&ref);
        if (as_run) {
            out[run_flags + i / 8] |= (uint8_t)(1u << (i % 8));
        }
        sx_store_u16(out + headers + 4 * i, (uint16_t)bitmap->keys[first + i]);
        sx_store_u16(out + headers + 4 * i + 2, (uint16_t)(container->cardinality - 1));
        if (has_offsets) {
            sx_store_u32(out + offsets + 4 * i, (uint32_t)position);
        }
        sx_rc_serialize(&ref, as_run, out + position);
        position += sx_rc_serialized_size(&ref, as_run);
    }
    return position;
}

sx_err sx_roaring_serialize(const sx_roaring* bitmap, void* buffer, size_t capacity, size_t* size) {
    size_t needed = sx_roaring_serialized_size(bitmap);
    if (needed > capacity) {
        return SX_ERR_LIMIT;
    }
    uint8_t* out = buffer;
    uint64_t buckets = 0;
    size_t position = 8;
    for (size_t first = 0; first < bitmap->count;) {
        size_t end = bucket_end(bitmap, first);
        sx_store_u32(out + position, (uint32_t)(bitmap->keys[first] >> 16));
        position += 4;
        position += serialize_bucket(bitmap, first, end, out + position);
        buckets++;
        first = end;
    }
    sx_store_u64(out, buckets);
    *size = position;
    return SX_OK;
}
