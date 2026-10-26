/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "common/roaring_container.h"

#include "common/bits.h"

#include <string.h>

#define LOW_VALUES 65536u

/* ---------------------------------------------------------------------------------------- */
/* Access to container data                                                                 */
/* ---------------------------------------------------------------------------------------- */

static inline uint16_t value_at(const uint8_t* data, uint32_t index) {
    return sx_load_u16(data + 2 * (size_t)index);
}

static inline void set_value_at(uint8_t* data, uint32_t index, uint16_t value) {
    sx_store_u16(data + 2 * (size_t)index, value);
}

static inline uint64_t word_at(const uint8_t* data, uint32_t index) {
    return sx_load_u64(data + 8 * (size_t)index);
}

/* First and last value of run `index`; the last is clipped to 65535. */
static inline void run_at(const uint8_t* data, uint32_t index, uint32_t* first, uint32_t* last) {
    uint32_t start = value_at(data, 2 * index);
    uint32_t end = start + value_at(data, 2 * index + 1);
    *first = start;
    *last = end > LOW_VALUES - 1 ? LOW_VALUES - 1 : end;
}

static inline void set_run_at(uint8_t* data, uint32_t index, uint32_t first, uint32_t last) {
    set_value_at(data, 2 * index, (uint16_t)first);
    set_value_at(data, 2 * index + 1, (uint16_t)(last - first));
}

/* Index of the first value >= low, or `count` if there is none. */
static uint32_t array_lower_bound(const uint8_t* data, uint32_t count, uint16_t low) {
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (value_at(data, mid) < low) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* Index of the last run starting at or before `low`, or -1 if there is none. */
static int64_t run_floor(const uint8_t* data, uint32_t count, uint16_t low) {
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (value_at(data, 2 * mid) <= low) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (int64_t)lo - 1;
}

/* ---------------------------------------------------------------------------------------- */
/* Word arrays (the common form for operations between different container types)          */
/* ---------------------------------------------------------------------------------------- */

static void set_range_in_words(uint64_t* words, uint32_t first, uint32_t last) {
    uint32_t first_word = first >> 6;
    uint32_t last_word = last >> 6;
    uint64_t first_mask = UINT64_MAX << (first & 63);
    uint64_t last_mask = UINT64_MAX >> (63 - (last & 63));
    if (first_word == last_word) {
        words[first_word] |= first_mask & last_mask;
        return;
    }
    words[first_word] |= first_mask;
    for (uint32_t i = first_word + 1; i < last_word; i++) {
        words[i] = UINT64_MAX;
    }
    words[last_word] |= last_mask;
}

static void to_words(const sx_rc_ref* container, uint64_t* words) {
    if (container->type == SX_RC_BITMAP) {
        memcpy(words, container->data, SX_RC_BITMAP_BYTES);
        return;
    }
    memset(words, 0, SX_RC_BITMAP_BYTES);
    if (container->type == SX_RC_ARRAY) {
        for (uint32_t i = 0; i < container->count; i++) {
            uint16_t low = value_at(container->data, i);
            words[low >> 6] |= UINT64_C(1) << (low & 63);
        }
        return;
    }
    for (uint32_t i = 0; i < container->count; i++) {
        uint32_t first;
        uint32_t last;
        run_at(container->data, i, &first, &last);
        set_range_in_words(words, first, last);
    }
}

/* Position of the first set (or, with `invert`, clear) bit at or after `position`. */
static uint32_t next_bit(const uint64_t* words, uint32_t position, bool invert) {
    if (position >= LOW_VALUES) {
        return LOW_VALUES;
    }
    uint32_t index = position >> 6;
    uint64_t flip = invert ? UINT64_MAX : 0;
    uint64_t word = (words[index] ^ flip) & (UINT64_MAX << (position & 63));
    while (word == 0) {
        index++;
        if (index == SX_RC_BITMAP_WORDS) {
            return LOW_VALUES;
        }
        word = words[index] ^ flip;
    }
    return index * 64 + sx_trailing_zeros_u64(word);
}

/* Writes the runs of `words` as (start, length - 1) pairs; returns how many. */
static uint32_t extract_runs(const uint64_t* words, uint8_t* out) {
    uint32_t count = 0;
    uint32_t position = 0;
    for (;;) {
        uint32_t first = next_bit(words, position, false);
        if (first == LOW_VALUES) {
            break;
        }
        uint32_t end = next_bit(words, first, true);
        set_run_at(out, count, first, end - 1);
        count++;
        position = end;
    }
    return count;
}

/* Writes the set positions of `words` as u16 values; returns how many. */
static uint32_t extract_values(const uint64_t* words, uint8_t* out) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < SX_RC_BITMAP_WORDS; i++) {
        uint64_t word = words[i];
        while (word != 0) {
            set_value_at(out, count, (uint16_t)(i * 64 + sx_trailing_zeros_u64(word)));
            count++;
            word &= word - 1;
        }
    }
    return count;
}

static void count_words(const uint64_t* words, uint32_t* cardinality, uint32_t* runs) {
    uint32_t total = 0;
    uint32_t starts = 0;
    uint64_t carry = 0;
    for (uint32_t i = 0; i < SX_RC_BITMAP_WORDS; i++) {
        uint64_t word = words[i];
        total += sx_popcount_u64(word);
        starts += sx_popcount_u64(word & ~((word << 1) | carry));
        carry = word >> 63;
    }
    *cardinality = total;
    *runs = starts;
}

/* ---------------------------------------------------------------------------------------- */
/* Allocation                                                                               */
/* ---------------------------------------------------------------------------------------- */

size_t sx_rc_data_size(uint8_t type, uint32_t count) {
    switch (type) {
        case SX_RC_ARRAY:
            return 2 * (size_t)count;
        case SX_RC_BITMAP:
            return SX_RC_BITMAP_BYTES;
        case SX_RC_RUN:
            return 4 * (size_t)count;
        default:
            return 0;
    }
}

void sx_rc_release(sx_rc* container, const sx_allocator* allocator) {
    sx_release(allocator, container->data, sx_rc_data_size(container->type, container->capacity));
    memset(container, 0, sizeof(*container));
}

/* Makes `container` an empty container of `type` with room for `capacity` values or runs. */
static sx_err allocate_container(sx_rc* container, uint8_t type, uint32_t capacity,
                                 const sx_allocator* allocator) {
    if (type == SX_RC_BITMAP) {
        capacity = SX_RC_BITMAP_WORDS;
    } else if (capacity == 0) {
        capacity = 1;
    }
    void* data = sx_allocate(allocator, sx_rc_data_size(type, capacity));
    if (data == NULL) {
        return SX_ERR_NOMEM;
    }
    container->type = type;
    container->cardinality = 0;
    container->count = type == SX_RC_BITMAP ? SX_RC_BITMAP_WORDS : 0;
    container->capacity = capacity;
    container->data = data;
    return SX_OK;
}

/* Grows the array or run storage to hold at least `needed` entries. */
static sx_err ensure_capacity(sx_rc* container, uint32_t needed, uint32_t limit,
                              const sx_allocator* allocator) {
    if (needed <= container->capacity) {
        return SX_OK;
    }
    uint32_t capacity = container->capacity < 4 ? 4 : container->capacity * 2;
    if (capacity < needed) {
        capacity = needed;
    }
    if (capacity > limit) {
        capacity = limit;
    }
    size_t old_size = sx_rc_data_size(container->type, container->capacity);
    void* data = sx_allocate(allocator, sx_rc_data_size(container->type, capacity));
    if (data == NULL) {
        return SX_ERR_NOMEM;
    }
    memcpy(data, container->data, sx_rc_data_size(container->type, container->count));
    sx_release(allocator, container->data, old_size);
    container->data = data;
    container->capacity = capacity;
    return SX_OK;
}

/*
 * Builds the smallest container holding exactly the bits of `words`: a run container when
 * that is smallest, otherwise an array up to SX_RC_ARRAY_MAX values, otherwise a bitmap.
 * An empty result has cardinality 0 and no data.
 */
static sx_err from_words(sx_rc* out, const uint64_t* words, const sx_allocator* allocator) {
    memset(out, 0, sizeof(*out));
    uint32_t cardinality;
    uint32_t runs;
    count_words(words, &cardinality, &runs);
    if (cardinality == 0) {
        return SX_OK;
    }
    size_t other_size =
        cardinality <= SX_RC_ARRAY_MAX ? 2 * (size_t)cardinality : SX_RC_BITMAP_BYTES;
    sx_err err;
    if (4 * (size_t)runs < other_size) {
        err = allocate_container(out, SX_RC_RUN, runs, allocator);
        if (err == SX_OK) {
            out->count = extract_runs(words, out->data);
        }
    } else if (cardinality <= SX_RC_ARRAY_MAX) {
        err = allocate_container(out, SX_RC_ARRAY, cardinality, allocator);
        if (err == SX_OK) {
            out->count = extract_values(words, out->data);
        }
    } else {
        err = allocate_container(out, SX_RC_BITMAP, 0, allocator);
        if (err == SX_OK) {
            memcpy(out->data, words, SX_RC_BITMAP_BYTES);
        }
    }
    if (err == SX_OK) {
        out->cardinality = cardinality;
    }
    return err;
}

/* Replaces a container with the bitmap form of its contents. */
static sx_err convert_to_bitmap(sx_rc* container, const sx_allocator* allocator) {
    sx_rc converted;
    sx_err err = allocate_container(&converted, SX_RC_BITMAP, 0, allocator);
    if (err != SX_OK) {
        return err;
    }
    sx_rc_ref ref = sx_rc_as_ref(container);
    to_words(&ref, converted.data);
    converted.cardinality = container->cardinality;
    sx_rc_release(container, allocator);
    *container = converted;
    return SX_OK;
}

/* ---------------------------------------------------------------------------------------- */
/* Reading                                                                                  */
/* ---------------------------------------------------------------------------------------- */

bool sx_rc_contains(const sx_rc_ref* container, uint16_t low) {
    switch (container->type) {
        case SX_RC_ARRAY: {
            uint32_t index = array_lower_bound(container->data, container->count, low);
            return index < container->count && value_at(container->data, index) == low;
        }
        case SX_RC_BITMAP:
            return ((word_at(container->data, low >> 6) >> (low & 63)) & 1) != 0;
        default: {
            int64_t index = run_floor(container->data, container->count, low);
            if (index < 0) {
                return false;
            }
            uint32_t first;
            uint32_t last;
            run_at(container->data, (uint32_t)index, &first, &last);
            return low <= last;
        }
    }
}

uint16_t sx_rc_minimum(const sx_rc_ref* container) {
    if (container->count == 0) {
        return 0;
    }
    switch (container->type) {
        case SX_RC_ARRAY:
            return value_at(container->data, 0);
        case SX_RC_BITMAP:
            for (uint32_t i = 0; i < SX_RC_BITMAP_WORDS; i++) {
                uint64_t word = word_at(container->data, i);
                if (word != 0) {
                    return (uint16_t)(i * 64 + sx_trailing_zeros_u64(word));
                }
            }
            return 0;
        default:
            return value_at(container->data, 0);
    }
}

uint16_t sx_rc_maximum(const sx_rc_ref* container) {
    if (container->count == 0) {
        return 0;
    }
    switch (container->type) {
        case SX_RC_ARRAY:
            return value_at(container->data, container->count - 1);
        case SX_RC_BITMAP:
            for (uint32_t i = SX_RC_BITMAP_WORDS; i > 0; i--) {
                uint64_t word = word_at(container->data, i - 1);
                if (word != 0) {
                    return (uint16_t)((i - 1) * 64 + 63 - (uint32_t)__builtin_clzll(word));
                }
            }
            return 0;
        default: {
            uint32_t first;
            uint32_t last;
            run_at(container->data, container->count - 1, &first, &last);
            return (uint16_t)last;
        }
    }
}

size_t sx_rc_next(const sx_rc_ref* container, sx_rc_cursor* cursor, uint64_t high, uint64_t* values,
                  size_t capacity) {
    size_t written = 0;
    switch (container->type) {
        case SX_RC_ARRAY:
            while (written < capacity && cursor->index < container->count) {
                values[written++] = high | value_at(container->data, cursor->index);
                cursor->index++;
            }
            break;
        case SX_RC_BITMAP:
            /* cursor->index is the next word to load; cursor->word the bits left in the last. */
            while (written < capacity) {
                while (cursor->word == 0) {
                    if (cursor->index >= SX_RC_BITMAP_WORDS) {
                        return written;
                    }
                    cursor->word = word_at(container->data, cursor->index);
                    cursor->index++;
                }
                uint32_t base = (cursor->index - 1) * 64;
                values[written++] = high | (base + sx_trailing_zeros_u64(cursor->word));
                cursor->word &= cursor->word - 1;
            }
            break;
        default:
            /*
             * cursor->word is the smallest value that may come next. Values therefore only
             * increase, and a container yields at most 65536 of them even when damaged
             * runs overlap, so iterating any serialized bytes takes bounded time.
             */
            while (written < capacity && cursor->index < container->count) {
                uint32_t first;
                uint32_t last;
                run_at(container->data, cursor->index, &first, &last);
                uint64_t low = first > cursor->word ? first : cursor->word;
                if (low > last) {
                    cursor->index++;
                    continue;
                }
                values[written++] = high | low;
                cursor->word = low + 1;
            }
            break;
    }
    return written;
}

/* ---------------------------------------------------------------------------------------- */
/* Changes                                                                                  */
/* ---------------------------------------------------------------------------------------- */

sx_err sx_rc_init_single(sx_rc* container, uint16_t low, const sx_allocator* allocator) {
    sx_err err = allocate_container(container, SX_RC_ARRAY, 4, allocator);
    if (err != SX_OK) {
        return err;
    }
    set_value_at(container->data, 0, low);
    container->count = 1;
    container->cardinality = 1;
    return SX_OK;
}

static sx_err bitmap_add(sx_rc* container, uint16_t low, bool* added) {
    uint64_t* words = container->data;
    uint64_t bit = UINT64_C(1) << (low & 63);
    *added = (words[low >> 6] & bit) == 0;
    if (*added) {
        words[low >> 6] |= bit;
        container->cardinality++;
    }
    return SX_OK;
}

static sx_err array_add(sx_rc* container, uint16_t low, bool* added,
                        const sx_allocator* allocator) {
    uint32_t index = array_lower_bound(container->data, container->count, low);
    if (index < container->count && value_at(container->data, index) == low) {
        *added = false;
        return SX_OK;
    }
    if (container->count == SX_RC_ARRAY_MAX) {
        sx_err err = convert_to_bitmap(container, allocator);
        if (err != SX_OK) {
            return err;
        }
        return bitmap_add(container, low, added);
    }
    sx_err err = ensure_capacity(container, container->count + 1, SX_RC_ARRAY_MAX, allocator);
    if (err != SX_OK) {
        return err;
    }
    uint8_t* data = container->data;
    memmove(data + 2 * ((size_t)index + 1), data + 2 * (size_t)index,
            2 * (size_t)(container->count - index));
    set_value_at(data, index, low);
    container->count++;
    container->cardinality++;
    *added = true;
    return SX_OK;
}

static sx_err run_add(sx_rc* container, uint16_t low, bool* added, const sx_allocator* allocator) {
    uint8_t* data = container->data;
    int64_t floor = run_floor(data, container->count, low);
    uint32_t previous_first = 0;
    uint32_t previous_last = 0;
    if (floor >= 0) {
        run_at(data, (uint32_t)floor, &previous_first, &previous_last);
        if (low <= previous_last) {
            *added = false;
            return SX_OK;
        }
    }
    uint32_t next = (uint32_t)(floor + 1);
    bool extends_previous = floor >= 0 && previous_last + 1 == low;
    bool extends_next = next < container->count && value_at(data, 2 * next) == (uint32_t)low + 1;
    if (extends_previous && extends_next) {
        uint32_t next_first;
        uint32_t next_last;
        run_at(data, next, &next_first, &next_last);
        set_run_at(data, (uint32_t)floor, previous_first, next_last);
        memmove(data + 4 * (size_t)next, data + 4 * ((size_t)next + 1),
                4 * (size_t)(container->count - next - 1));
        container->count--;
    } else if (extends_previous) {
        set_run_at(data, (uint32_t)floor, previous_first, low);
    } else if (extends_next) {
        uint32_t next_first;
        uint32_t next_last;
        run_at(data, next, &next_first, &next_last);
        set_run_at(data, next, low, next_last);
    } else {
        if (container->count == SX_RC_RUN_MAX) {
            sx_err err = convert_to_bitmap(container, allocator);
            if (err != SX_OK) {
                return err;
            }
            return bitmap_add(container, low, added);
        }
        sx_err err = ensure_capacity(container, container->count + 1, SX_RC_RUN_MAX, allocator);
        if (err != SX_OK) {
            return err;
        }
        data = container->data;
        memmove(data + 4 * ((size_t)next + 1), data + 4 * (size_t)next,
                4 * (size_t)(container->count - next));
        set_run_at(data, next, low, low);
        container->count++;
    }
    container->cardinality++;
    *added = true;
    return SX_OK;
}

sx_err sx_rc_add(sx_rc* container, uint16_t low, bool* added, const sx_allocator* allocator) {
    switch (container->type) {
        case SX_RC_ARRAY:
            return array_add(container, low, added, allocator);
        case SX_RC_BITMAP:
            return bitmap_add(container, low, added);
        default:
            return run_add(container, low, added, allocator);
    }
}

sx_err sx_rc_add_range(sx_rc* container, uint16_t first, uint16_t last,
                       const sx_allocator* allocator) {
    if (container->cardinality == 0) {
        sx_rc_release(container, allocator);
        sx_err err = allocate_container(container, SX_RC_RUN, 1, allocator);
        if (err != SX_OK) {
            return err;
        }
        set_run_at(container->data, 0, first, last);
        container->count = 1;
        container->cardinality = (uint32_t)last - first + 1;
        return SX_OK;
    }
    uint64_t words[SX_RC_BITMAP_WORDS];
    sx_rc_ref ref = sx_rc_as_ref(container);
    to_words(&ref, words);
    set_range_in_words(words, first, last);
    sx_rc rebuilt;
    sx_err err = from_words(&rebuilt, words, allocator);
    if (err != SX_OK) {
        return err;
    }
    sx_rc_release(container, allocator);
    *container = rebuilt;
    return SX_OK;
}

sx_err sx_rc_copy(sx_rc* out, const sx_rc_ref* in, const sx_allocator* allocator) {
    memset(out, 0, sizeof(*out));
    sx_err err = allocate_container(out, in->type, in->count, allocator);
    if (err != SX_OK) {
        return err;
    }
    memcpy(out->data, in->data, sx_rc_data_size(in->type, in->count));
    out->count = in->count;
    out->cardinality = in->cardinality;
    return SX_OK;
}

/* ---------------------------------------------------------------------------------------- */
/* Set operations                                                                           */
/* ---------------------------------------------------------------------------------------- */

/* Keeps the values of the array `values` that are (or, with keep_present false, are not) in
 * `other`. */
static sx_err filter_array(sx_rc* out, const sx_rc_ref* values, const sx_rc_ref* other,
                           bool keep_present, const sx_allocator* allocator) {
    memset(out, 0, sizeof(*out));
    sx_err err = allocate_container(out, SX_RC_ARRAY, values->count, allocator);
    if (err != SX_OK) {
        return err;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < values->count; i++) {
        uint16_t low = value_at(values->data, i);
        if (sx_rc_contains(other, low) == keep_present) {
            set_value_at(out->data, kept++, low);
        }
    }
    out->count = kept;
    out->cardinality = kept;
    if (kept == 0) {
        sx_rc_release(out, allocator);
    }
    return SX_OK;
}

static sx_err intersect_arrays(sx_rc* out, const sx_rc_ref* a, const sx_rc_ref* b,
                               const sx_allocator* allocator) {
    memset(out, 0, sizeof(*out));
    uint32_t capacity = a->count < b->count ? a->count : b->count;
    sx_err err = allocate_container(out, SX_RC_ARRAY, capacity, allocator);
    if (err != SX_OK) {
        return err;
    }
    uint32_t i = 0;
    uint32_t j = 0;
    uint32_t kept = 0;
    while (i < a->count && j < b->count) {
        uint16_t x = value_at(a->data, i);
        uint16_t y = value_at(b->data, j);
        if (x < y) {
            i++;
        } else if (y < x) {
            j++;
        } else {
            if (kept < capacity) {
                set_value_at(out->data, kept++, x);
            }
            i++;
            j++;
        }
    }
    out->count = kept;
    out->cardinality = kept;
    if (kept == 0) {
        sx_rc_release(out, allocator);
    }
    return SX_OK;
}

sx_err sx_rc_and(sx_rc* out, const sx_rc_ref* a, const sx_rc_ref* b,
                 const sx_allocator* allocator) {
    if (a->type == SX_RC_ARRAY && b->type == SX_RC_ARRAY) {
        return intersect_arrays(out, a, b, allocator);
    }
    if (a->type == SX_RC_ARRAY) {
        return filter_array(out, a, b, true, allocator);
    }
    if (b->type == SX_RC_ARRAY) {
        return filter_array(out, b, a, true, allocator);
    }
    uint64_t words[SX_RC_BITMAP_WORDS];
    uint64_t other[SX_RC_BITMAP_WORDS];
    to_words(a, words);
    to_words(b, other);
    for (uint32_t i = 0; i < SX_RC_BITMAP_WORDS; i++) {
        words[i] &= other[i];
    }
    return from_words(out, words, allocator);
}

sx_err sx_rc_or(sx_rc* out, const sx_rc_ref* a, const sx_rc_ref* b, const sx_allocator* allocator) {
    uint64_t words[SX_RC_BITMAP_WORDS];
    uint64_t other[SX_RC_BITMAP_WORDS];
    to_words(a, words);
    to_words(b, other);
    for (uint32_t i = 0; i < SX_RC_BITMAP_WORDS; i++) {
        words[i] |= other[i];
    }
    return from_words(out, words, allocator);
}

sx_err sx_rc_andnot(sx_rc* out, const sx_rc_ref* a, const sx_rc_ref* b,
                    const sx_allocator* allocator) {
    if (a->type == SX_RC_ARRAY) {
        return filter_array(out, a, b, false, allocator);
    }
    uint64_t words[SX_RC_BITMAP_WORDS];
    uint64_t other[SX_RC_BITMAP_WORDS];
    to_words(a, words);
    to_words(b, other);
    for (uint32_t i = 0; i < SX_RC_BITMAP_WORDS; i++) {
        words[i] &= ~other[i];
    }
    return from_words(out, words, allocator);
}

/* ---------------------------------------------------------------------------------------- */
/* Serialization                                                                            */
/* ---------------------------------------------------------------------------------------- */

static uint32_t count_runs(const sx_rc_ref* container) {
    switch (container->type) {
        case SX_RC_RUN:
            return container->count;
        case SX_RC_ARRAY: {
            uint32_t runs = container->count > 0 ? 1 : 0;
            for (uint32_t i = 1; i < container->count; i++) {
                if (value_at(container->data, i) != value_at(container->data, i - 1) + 1u) {
                    runs++;
                }
            }
            return runs;
        }
        default: {
            uint64_t words[SX_RC_BITMAP_WORDS];
            to_words(container, words);
            uint32_t cardinality;
            uint32_t runs;
            count_words(words, &cardinality, &runs);
            return runs;
        }
    }
}

/* Size of the array or bitmap form, which the portable format chooses by cardinality. */
static size_t plain_size(uint32_t cardinality) {
    return cardinality <= SX_RC_ARRAY_MAX ? 2 * (size_t)cardinality : SX_RC_BITMAP_BYTES;
}

bool sx_rc_serialize_as_run(const sx_rc_ref* container) {
    return 2 + 4 * (size_t)count_runs(container) < plain_size(container->cardinality);
}

size_t sx_rc_serialized_size(const sx_rc_ref* container, bool as_run) {
    if (as_run) {
        return 2 + 4 * (size_t)count_runs(container);
    }
    return plain_size(container->cardinality);
}

void sx_rc_serialize(const sx_rc_ref* container, bool as_run, uint8_t* out) {
    if (as_run) {
        uint32_t runs;
        if (container->type == SX_RC_RUN) {
            runs = container->count;
            memcpy(out + 2, container->data, 4 * (size_t)runs);
        } else {
            uint64_t words[SX_RC_BITMAP_WORDS];
            to_words(container, words);
            runs = extract_runs(words, out + 2);
        }
        sx_store_u16(out, (uint16_t)runs);
        return;
    }
    if (container->type == SX_RC_ARRAY) {
        memcpy(out, container->data, 2 * (size_t)container->count);
        return;
    }
    if (container->type == SX_RC_BITMAP && container->cardinality > SX_RC_ARRAY_MAX) {
        memcpy(out, container->data, SX_RC_BITMAP_BYTES);
        return;
    }
    /* `out` may be unaligned, so the words are built aside and copied. */
    uint64_t words[SX_RC_BITMAP_WORDS];
    to_words(container, words);
    if (container->cardinality > SX_RC_ARRAY_MAX) {
        memcpy(out, words, SX_RC_BITMAP_BYTES);
    } else {
        extract_values(words, out);
    }
}
