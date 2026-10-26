/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sx_test.h"

#include "common/allocator.h"
#include "common/compiler.h"
#include "common/rng.h"
#include "common/roaring.h"
#include "common/xxhash.h"
#include "mem/arena.h"
#include "mem/mem.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------------- */
/* Reference model: a sorted array of distinct values                                      */
/* ---------------------------------------------------------------------------------------- */

typedef struct value_set {
    uint64_t* values;
    size_t count;
    size_t capacity;
} value_set;

static void set_push(value_set* set, uint64_t value) {
    if (set->count == set->capacity) {
        size_t capacity = set->capacity == 0 ? 256 : set->capacity * 2;
        uint64_t* grown = realloc(set->values, capacity * sizeof(uint64_t));
        if (grown == NULL) {
            abort();
        }
        set->values = grown;
        set->capacity = capacity;
    }
    set->values[set->count++] = value;
}

static int compare_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

static void set_normalize(value_set* set) {
    if (set->count == 0) {
        return;
    }
    qsort(set->values, set->count, sizeof(uint64_t), compare_u64);
    size_t kept = 1;
    for (size_t i = 1; i < set->count; i++) {
        if (set->values[i] != set->values[kept - 1]) {
            set->values[kept++] = set->values[i];
        }
    }
    set->count = kept;
}

static void set_free(value_set* set) {
    free(set->values);
    memset(set, 0, sizeof(*set));
}

typedef enum set_operation {
    SET_AND,
    SET_OR,
    SET_ANDNOT,
} set_operation;

static void set_combine(value_set* out, const value_set* a, const value_set* b,
                        set_operation operation) {
    size_t i = 0;
    size_t j = 0;
    while (i < a->count || j < b->count) {
        bool take_a = j == b->count || (i < a->count && a->values[i] < b->values[j]);
        bool take_b = i == a->count || (j < b->count && b->values[j] < a->values[i]);
        if (take_a) {
            if (operation != SET_AND) {
                set_push(out, a->values[i]);
            }
            i++;
        } else if (take_b) {
            if (operation == SET_OR) {
                set_push(out, b->values[j]);
            }
            j++;
        } else {
            if (operation != SET_ANDNOT) {
                set_push(out, a->values[i]);
            }
            i++;
            j++;
        }
    }
}

/* ---------------------------------------------------------------------------------------- */
/* Helpers                                                                                  */
/* ---------------------------------------------------------------------------------------- */

/* Checks iteration, cardinality, minimum, maximum, and membership against `set`. */
static bool matches(sx_roaring_ref ref, const value_set* set) {
    if (sx_roaring_cardinality(ref) != set->count) {
        return false;
    }
    if (sx_roaring_is_empty(ref) != (set->count == 0)) {
        return false;
    }
    sx_roaring_iterator iterator;
    sx_roaring_iterator_init(&iterator, ref);
    uint64_t batch[333];
    size_t seen = 0;
    for (;;) {
        size_t got = sx_roaring_iterator_next(&iterator, batch, SX_ARRAY_SIZE(batch));
        if (got == 0) {
            break;
        }
        for (size_t i = 0; i < got; i++) {
            if (seen >= set->count || batch[i] != set->values[seen]) {
                return false;
            }
            seen++;
        }
    }
    if (seen != set->count) {
        return false;
    }
    uint64_t value;
    if (set->count == 0) {
        return !sx_roaring_minimum(ref, &value) && !sx_roaring_maximum(ref, &value);
    }
    if (!sx_roaring_minimum(ref, &value) || value != set->values[0]) {
        return false;
    }
    if (!sx_roaring_maximum(ref, &value) || value != set->values[set->count - 1]) {
        return false;
    }
    size_t step = set->count / 500 + 1;
    for (size_t i = 0; i < set->count; i += step) {
        uint64_t present = set->values[i];
        if (!sx_roaring_contains(ref, present)) {
            return false;
        }
        bool next_present = i + 1 < set->count && set->values[i + 1] == present + 1;
        if (present != UINT64_MAX && sx_roaring_contains(ref, present + 1) != next_present) {
            return false;
        }
    }
    return true;
}

/* Adds random values and ranges to both a bitmap and a set. */
static bool random_fill(sx_rng* rng, sx_roaring* bitmap, value_set* set) {
    static const uint64_t bases[] = {
        0,
        UINT64_C(1) << 16,
        UINT64_C(5) << 16,
        UINT64_C(1) << 32,
        (UINT64_C(1) << 32) + (UINT64_C(2) << 16),
        UINT64_MAX - 400000,
    };
    int segments = 2 + (int)sx_rng_below(rng, 5);
    for (int s = 0; s < segments; s++) {
        uint64_t base = bases[sx_rng_below(rng, SX_ARRAY_SIZE(bases))];
        switch (sx_rng_below(rng, 4)) {
            case 0: /* sparse values */
                for (int i = 0; i < 60; i++) {
                    uint64_t value = base + sx_rng_below(rng, 131072);
                    set_push(set, value);
                    if (sx_roaring_add(bitmap, value) != SX_OK) {
                        return false;
                    }
                }
                break;
            case 1: { /* dense values with a stride */
                uint64_t stride = 1 + sx_rng_below(rng, 4);
                uint64_t start = base + sx_rng_below(rng, 65536);
                uint64_t span = 5000 + sx_rng_below(rng, 12000);
                for (uint64_t value = start; value < start + span; value += stride) {
                    set_push(set, value);
                    if (sx_roaring_add(bitmap, value) != SX_OK) {
                        return false;
                    }
                }
                break;
            }
            case 2: { /* a range */
                uint64_t start = base + sx_rng_below(rng, 100000);
                uint64_t length = 1 + sx_rng_below(rng, 80000);
                for (uint64_t value = start; value < start + length; value++) {
                    set_push(set, value);
                }
                if (sx_roaring_add_range(bitmap, start, start + length) != SX_OK) {
                    return false;
                }
                break;
            }
            default: { /* single values next to each other, merging runs */
                uint64_t start = base + sx_rng_below(rng, 65536);
                for (uint64_t k = 0; k < 40; k++) {
                    uint64_t value = start + (k % 2 == 0 ? k : 80 - k);
                    set_push(set, value);
                    if (sx_roaring_add(bitmap, value) != SX_OK) {
                        return false;
                    }
                }
                break;
            }
        }
    }
    set_normalize(set);
    return true;
}

/* Serializes a bitmap into newly allocated memory and opens a view on it. */
static uint8_t* serialize_to_view(const sx_roaring* bitmap, sx_roaring_view* view, size_t* size) {
    size_t capacity = sx_roaring_serialized_size(bitmap);
    uint8_t* buffer = malloc(capacity);
    if (buffer == NULL) {
        return NULL;
    }
    if (sx_roaring_serialize(bitmap, buffer, capacity, size) != SX_OK || *size != capacity ||
        sx_roaring_view_open(view, buffer, *size) != SX_OK) {
        free(buffer);
        return NULL;
    }
    return buffer;
}

static size_t decode_hex(const char* hex, uint8_t* out, size_t capacity) {
    size_t length = strlen(hex) / 2;
    if (length > capacity) {
        return 0;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned value = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[2 * i + (size_t)k];
            unsigned digit = c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10);
            value = value * 16 + digit;
        }
        out[i] = (uint8_t)value;
    }
    return length;
}

/* ---------------------------------------------------------------------------------------- */
/* Tests                                                                                    */
/* ---------------------------------------------------------------------------------------- */

static void test_empty(void) {
    sx_roaring bitmap;
    sx_roaring_init(&bitmap, &sx_heap_allocator);
    value_set empty = {0};
    SX_CHECK(matches(sx_roaring_ref_bitmap(&bitmap), &empty));
    SX_CHECK(!sx_roaring_contains(sx_roaring_ref_bitmap(&bitmap), 0));
    SX_CHECK_EQ_U64(sx_roaring_serialized_size(&bitmap), 8);
    sx_roaring_view view;
    size_t size = 0;
    uint8_t* bytes = serialize_to_view(&bitmap, &view, &size);
    SX_CHECK(bytes != NULL);
    SX_CHECK(matches(sx_roaring_ref_view(&view), &empty));
    free(bytes);
    SX_CHECK(sx_roaring_add_range(&bitmap, 10, 10) == SX_OK);
    SX_CHECK(matches(sx_roaring_ref_bitmap(&bitmap), &empty));
    sx_roaring_clear(&bitmap);
}

static void test_container_conversions(void) {
    sx_roaring bitmap;
    sx_roaring_init(&bitmap, &sx_heap_allocator);
    value_set set = {0};
    /* Checks accumulate in `ok`, so the memory is freed before the test can stop. */
    bool ok = true;
    /* An array that grows past 4096 values becomes a bitmap. */
    for (uint64_t value = 0; value < UINT64_C(2) * 4100; value += 2) {
        set_push(&set, value);
        ok = ok && sx_roaring_add(&bitmap, value) == SX_OK;
    }
    ok = ok && sx_roaring_add(&bitmap, 0) == SX_OK; /* already present */
    /* Runs that touch, extend, and merge. */
    uint64_t base = UINT64_C(3) << 16;
    ok = ok && sx_roaring_add_range(&bitmap, base + 10, base + 20) == SX_OK;
    ok = ok && sx_roaring_add_range(&bitmap, base + 22, base + 30) == SX_OK;
    ok = ok && sx_roaring_add(&bitmap, base + 20) == SX_OK; /* extends the first run */
    ok = ok && sx_roaring_add(&bitmap, base + 9) == SX_OK;  /* extends it at the front */
    ok = ok && sx_roaring_add(&bitmap, base + 21) == SX_OK; /* merges both runs */
    ok = ok && sx_roaring_add(&bitmap, base + 40) == SX_OK; /* a new run */
    ok = ok && sx_roaring_add(&bitmap, base + 15) == SX_OK; /* inside a run */
    for (uint64_t value = base + 9; value < base + 30; value++) {
        set_push(&set, value);
    }
    set_push(&set, base + 40);
    /* The last values of the space. */
    ok = ok && sx_roaring_add_range(&bitmap, UINT64_MAX - 5, UINT64_MAX) == SX_OK;
    ok = ok && sx_roaring_add(&bitmap, UINT64_MAX) == SX_OK;
    for (uint64_t value = UINT64_MAX - 5; value != 0; value++) {
        set_push(&set, value);
    }
    set_normalize(&set);
    ok = ok && matches(sx_roaring_ref_bitmap(&bitmap), &set);

    sx_roaring_view view;
    size_t size = 0;
    uint8_t* bytes = serialize_to_view(&bitmap, &view, &size);
    ok = ok && bytes != NULL && matches(sx_roaring_ref_view(&view), &set);
    free(bytes);
    sx_roaring_clear(&bitmap);
    set_free(&set);
    SX_CHECK(ok);
}

/* Many single values in alternating positions exceed the run limit and become a bitmap. */
static void test_many_runs(void) {
    sx_roaring bitmap;
    sx_roaring_init(&bitmap, &sx_heap_allocator);
    value_set set = {0};
    bool ok = sx_roaring_add_range(&bitmap, 0, 2) == SX_OK;
    set_push(&set, 0);
    set_push(&set, 1);
    for (uint64_t value = 4; value < 20000; value += 3) {
        ok = ok && sx_roaring_add(&bitmap, value) == SX_OK;
        set_push(&set, value);
    }
    ok = ok && matches(sx_roaring_ref_bitmap(&bitmap), &set);
    sx_roaring_clear(&bitmap);
    set_free(&set);
    SX_CHECK(ok);
}

/* The bytes must equal those of CRoaring (through pyroaring 1.2.0, after run_optimize). */
static void test_matches_croaring_bytes(void) {
    typedef struct reference {
        size_t size;
        uint64_t hash;
    } reference;
    static const reference references[] = {
        {46, UINT64_C(0x65e93abe45a306f8)},    /* 1, 2, 3, 1000, 3 * 65536 + 7 */
        {37, UINT64_C(0xe42ef92b34ecab95)},    /* [0, 100000) */
        {74, UINT64_C(0x702e9ae5642141ce)},    /* 5, 2^32 + 9, 2^40 */
        {8220, UINT64_C(0x26accb3faddb3914)},  /* 0, 3, ..., 29997 */
        {11763, UINT64_C(0x11ed0571b52e0c97)}, /* a mixture, below */
    };
    for (size_t case_index = 0; case_index < SX_ARRAY_SIZE(references); case_index++) {
        sx_roaring bitmap;
        sx_roaring_init(&bitmap, &sx_heap_allocator);
        sx_err err = SX_OK;
        switch (case_index) {
            case 0: {
                const uint64_t values[] = {1, 2, 3, 1000, UINT64_C(3) * 65536 + 7};
                for (size_t i = 0; i < SX_ARRAY_SIZE(values) && err == SX_OK; i++) {
                    err = sx_roaring_add(&bitmap, values[i]);
                }
                break;
            }
            case 1:
                err = sx_roaring_add_range(&bitmap, 0, 100000);
                break;
            case 2:
                err = sx_roaring_add(&bitmap, 5);
                if (err == SX_OK) {
                    err = sx_roaring_add(&bitmap, (UINT64_C(1) << 32) + 9);
                }
                if (err == SX_OK) {
                    err = sx_roaring_add(&bitmap, UINT64_C(1) << 40);
                }
                break;
            case 3:
                for (uint64_t value = 0; value < 30000 && err == SX_OK; value += 3) {
                    err = sx_roaring_add(&bitmap, value);
                }
                break;
            default:
                for (uint64_t value = 0; value < 5000 && err == SX_OK; value += 2) {
                    err = sx_roaring_add(&bitmap, value);
                }
                if (err == SX_OK) {
                    err = sx_roaring_add_range(&bitmap, 70000, 140000);
                }
                for (uint64_t k = 0; k < 3 && err == SX_OK; k++) {
                    const uint64_t extra[] = {0, 1, 70000};
                    err = sx_roaring_add(&bitmap, (UINT64_C(1) << 33) + extra[k]);
                }
                for (uint64_t value = 200000; value < 210000 && err == SX_OK; value += 3) {
                    err = sx_roaring_add(&bitmap, value);
                }
                break;
        }
        SX_CHECK(err == SX_OK);
        sx_roaring_view view;
        size_t size = 0;
        uint8_t* bytes = serialize_to_view(&bitmap, &view, &size);
        SX_CHECK(bytes != NULL);
        SX_CHECK_EQ_U64(size, references[case_index].size);
        SX_CHECK_EQ_U64(sx_xxh64(bytes, size, 0), references[case_index].hash);
        free(bytes);
        sx_roaring_clear(&bitmap);
    }
}

/* Views of bytes written by CRoaring read correctly. */
static void test_reads_croaring_bytes(void) {
    static const char* const hex[] = {
        "0100000000000000000000003a3000000200000000000300030000001800000020000000010002000300e8"
        "030700",
        "0100000000000000000000003b300100030000ffff01009f8601000000ffff010000009f86",
        "0300000000000000000000003a3000000100000000000000100000000500010000003a30000001000000"
        "00000000100000000900000100003a3000000100000000000000100000000000",
    };
    uint8_t bytes[128];
    for (size_t i = 0; i < SX_ARRAY_SIZE(hex); i++) {
        size_t size = decode_hex(hex[i], bytes, sizeof(bytes));
        SX_CHECK(size > 0);
        sx_roaring_view view;
        SX_CHECK(sx_roaring_view_open(&view, bytes, size) == SX_OK);
        value_set set = {0};
        if (i == 0) {
            const uint64_t values[] = {1, 2, 3, 1000, UINT64_C(3) * 65536 + 7};
            for (size_t k = 0; k < SX_ARRAY_SIZE(values); k++) {
                set_push(&set, values[k]);
            }
        } else if (i == 1) {
            for (uint64_t value = 0; value < 100000; value++) {
                set_push(&set, value);
            }
        } else {
            set_push(&set, 5);
            set_push(&set, (UINT64_C(1) << 32) + 9);
            set_push(&set, UINT64_C(1) << 40);
        }
        bool ok = matches(sx_roaring_ref_view(&view), &set);
        set_free(&set);
        SX_CHECK(ok);
    }
}

/* Random bitmaps: every operation on every combination of forms matches the model. */
static void test_random_operations(void) {
    sx_rng rng;
    sx_rng_seed(&rng, 2026);
    for (int round = 0; round < 16; round++) {
        sx_roaring a;
        sx_roaring b;
        sx_roaring_init(&a, &sx_heap_allocator);
        sx_roaring_init(&b, &sx_heap_allocator);
        value_set set_a = {0};
        value_set set_b = {0};
        SX_CHECK(random_fill(&rng, &a, &set_a));
        SX_CHECK(random_fill(&rng, &b, &set_b));
        SX_CHECK(matches(sx_roaring_ref_bitmap(&a), &set_a));

        sx_roaring_view view_a;
        sx_roaring_view view_b;
        size_t size_a = 0;
        size_t size_b = 0;
        uint8_t* bytes_a = serialize_to_view(&a, &view_a, &size_a);
        uint8_t* bytes_b = serialize_to_view(&b, &view_b, &size_b);
        SX_CHECK(bytes_a != NULL && bytes_b != NULL);
        SX_CHECK(matches(sx_roaring_ref_view(&view_a), &set_a));

        sx_roaring_ref forms_a[] = {sx_roaring_ref_bitmap(&a), sx_roaring_ref_view(&view_a)};
        sx_roaring_ref forms_b[] = {sx_roaring_ref_bitmap(&b), sx_roaring_ref_view(&view_b)};
        for (int operation = SET_AND; operation <= SET_ANDNOT; operation++) {
            value_set expected = {0};
            set_combine(&expected, &set_a, &set_b, (set_operation)operation);
            for (int x = 0; x < 2; x++) {
                for (int y = 0; y < 2; y++) {
                    sx_roaring result;
                    sx_roaring_init(&result, &sx_heap_allocator);
                    sx_err err;
                    if (operation == SET_AND) {
                        err = sx_roaring_and(&result, forms_a[x], forms_b[y]);
                    } else if (operation == SET_OR) {
                        err = sx_roaring_or(&result, forms_a[x], forms_b[y]);
                    } else {
                        err = sx_roaring_andnot(&result, forms_a[x], forms_b[y]);
                    }
                    bool ok = err == SX_OK && matches(sx_roaring_ref_bitmap(&result), &expected);
                    sx_roaring_view view_result;
                    size_t size_result = 0;
                    uint8_t* bytes_result = serialize_to_view(&result, &view_result, &size_result);
                    ok = ok && bytes_result != NULL &&
                         matches(sx_roaring_ref_view(&view_result), &expected);
                    free(bytes_result);
                    sx_roaring_clear(&result);
                    if (!ok) {
                        set_free(&expected);
                    }
                    SX_CHECK(ok);
                }
            }
            set_free(&expected);
        }

        /* Copy and in-place union. */
        sx_roaring copy;
        sx_roaring_init(&copy, &sx_heap_allocator);
        SX_CHECK(sx_roaring_copy(&copy, sx_roaring_ref_view(&view_a)) == SX_OK);
        SX_CHECK(matches(sx_roaring_ref_bitmap(&copy), &set_a));
        SX_CHECK(sx_roaring_or_into(&copy, sx_roaring_ref_view(&view_b)) == SX_OK);
        value_set expected = {0};
        set_combine(&expected, &set_a, &set_b, SET_OR);
        bool ok = matches(sx_roaring_ref_bitmap(&copy), &expected);
        set_free(&expected);
        SX_CHECK(ok);
        SX_CHECK(sx_roaring_and(&copy, sx_roaring_ref_bitmap(&copy),
                                sx_roaring_ref_view(&view_b)) == SX_ERR_INVALID_ARG);

        sx_roaring_clear(&copy);
        free(bytes_a);
        free(bytes_b);
        sx_roaring_clear(&a);
        sx_roaring_clear(&b);
        set_free(&set_a);
        set_free(&set_b);
    }
}

/*
 * Damaged bytes are rejected or, if their structure still holds, read without leaving
 * the buffer (the sanitizer builds check the latter).
 */
static void test_damaged_views_stay_in_bounds(void) {
    sx_rng rng;
    sx_rng_seed(&rng, 99);
    sx_roaring bitmap;
    sx_roaring_init(&bitmap, &sx_heap_allocator);
    value_set set = {0};
    bool filled = random_fill(&rng, &bitmap, &set);
    set_free(&set); /* only the bitmap is needed here */
    SX_CHECK(filled);
    sx_roaring_view view;
    size_t size = 0;
    uint8_t* original = serialize_to_view(&bitmap, &view, &size);
    SX_CHECK(original != NULL);
    sx_roaring other;
    sx_roaring_init(&other, &sx_heap_allocator);
    SX_CHECK(sx_roaring_add_range(&other, 0, 300000) == SX_OK);

    uint64_t opened = 0;
    for (int round = 0; round < 1000; round++) {
        uint8_t* damaged = malloc(size);
        SX_CHECK(damaged != NULL);
        memcpy(damaged, original, size);
        size_t damaged_size = size;
        switch (sx_rng_below(&rng, 3)) {
            case 0:
                damaged_size = (size_t)sx_rng_below(&rng, size);
                break;
            case 1:
                for (int k = 0; k < 3; k++) {
                    /* Separate statements: the order of two calls in one expression is
                     * unspecified, and every compiler must damage the same bytes. */
                    size_t at = (size_t)sx_rng_below(&rng, size);
                    uint8_t bit = (uint8_t)(1u << sx_rng_below(&rng, 8));
                    damaged[at] ^= bit;
                }
                break;
            default: {
                /* Damage a header or a run, where structure lives. */
                size_t at = (size_t)sx_rng_below(&rng, size < 64 ? size : 64);
                damaged[at] = (uint8_t)sx_rng_next(&rng);
                break;
            }
        }
        sx_roaring_view damaged_view;
        if (sx_roaring_view_open(&damaged_view, damaged, damaged_size) == SX_OK) {
            opened++;
            sx_roaring_ref ref = sx_roaring_ref_view(&damaged_view);
            sx_roaring_iterator iterator;
            sx_roaring_iterator_init(&iterator, ref);
            uint64_t batch[512];
            while (sx_roaring_iterator_next(&iterator, batch, SX_ARRAY_SIZE(batch)) > 0) {
            }
            uint64_t value;
            sx_roaring_minimum(ref, &value);
            sx_roaring_maximum(ref, &value);
            for (int k = 0; k < 20; k++) {
                sx_roaring_contains(ref, sx_rng_next(&rng) % 400000);
            }
            sx_roaring result;
            sx_roaring_init(&result, &sx_heap_allocator);
            SX_CHECK(sx_roaring_and(&result, ref, sx_roaring_ref_bitmap(&other)) == SX_OK);
            SX_CHECK(sx_roaring_or(&result, sx_roaring_ref_bitmap(&other), ref) == SX_OK);
            SX_CHECK(sx_roaring_andnot(&result, ref, sx_roaring_ref_bitmap(&other)) == SX_OK);
            sx_roaring_clear(&result);
        }
        free(damaged);
    }
    SX_CHECK(opened > 0); /* some damage keeps the structure valid */
    free(original);
    sx_roaring_clear(&other);
    sx_roaring_clear(&bitmap);
}

/* Every byte is counted, and running out of memory leaves a usable bitmap. */
static void test_counted_allocators(void) {
    const uint64_t limit = UINT64_C(64) << 20;
    sx_mem_manager* manager = NULL;
    SX_CHECK(sx_mem_create(limit, limit - (UINT64_C(256) << 10), &manager) == SX_OK);
    sx_mem_heap heap = {manager, SX_MEM_POOL_INDEX};
    sx_allocator allocator = sx_mem_heap_allocator(&heap);
    sx_roaring bitmap;
    sx_roaring_init(&bitmap, &allocator);
    SX_CHECK(sx_roaring_add_range(&bitmap, 0, 1000000) == SX_OK);
    sx_mem_stats stats;
    sx_mem_get_stats(manager, &stats);
    SX_CHECK(stats.pool_used[SX_MEM_POOL_INDEX] > 0);

    /* Dense bitmap containers of 8 KB each soon exceed the 256 KB left. */
    sx_err err = SX_OK;
    for (uint64_t value = 0; value < 10000000 && err == SX_OK; value += 2) {
        err = sx_roaring_add(&bitmap, (UINT64_C(1) << 24) + value);
    }
    SX_CHECK(err == SX_ERR_NOMEM);
    SX_CHECK(sx_roaring_contains(sx_roaring_ref_bitmap(&bitmap), 999999));
    sx_roaring_clear(&bitmap);
    sx_mem_get_stats(manager, &stats);
    SX_CHECK_EQ_U64(stats.pool_used[SX_MEM_POOL_INDEX], 0);
    sx_mem_destroy(manager);
}

static void test_arena_allocator(void) {
    sx_mem_manager* manager = NULL;
    SX_CHECK(sx_mem_create(UINT64_C(64) << 20, 0, &manager) == SX_OK);
    sx_arena* arena = sx_arena_create(manager, SX_MEM_POOL_QUERY);
    SX_CHECK(arena != NULL);
    sx_allocator allocator = sx_arena_allocator(arena);
    sx_roaring a;
    sx_roaring b;
    sx_roaring result;
    sx_roaring_init(&a, &allocator);
    sx_roaring_init(&b, &allocator);
    sx_roaring_init(&result, &allocator);
    SX_CHECK(sx_roaring_add_range(&a, 0, 100000) == SX_OK);
    for (uint64_t value = 50000; value < 60000; value += 7) {
        SX_CHECK(sx_roaring_add(&b, value) == SX_OK);
    }
    SX_CHECK(sx_roaring_and(&result, sx_roaring_ref_bitmap(&a), sx_roaring_ref_bitmap(&b)) ==
             SX_OK);
    SX_CHECK_EQ_U64(sx_roaring_cardinality(sx_roaring_ref_bitmap(&result)), 1429);
    SX_CHECK(sx_arena_reserved_bytes(arena) > 0);
    sx_arena_destroy(arena);
    sx_mem_destroy(manager);
}

static void test_rejects_bad_structure(void) {
    sx_roaring bitmap;
    sx_roaring_init(&bitmap, &sx_heap_allocator);
    SX_CHECK(sx_roaring_add(&bitmap, 1) == SX_OK);
    SX_CHECK(sx_roaring_add(&bitmap, UINT64_C(1) << 32) == SX_OK);
    uint8_t bytes[256];
    size_t size = 0;
    SX_CHECK(sx_roaring_serialize(&bitmap, bytes, 4, &size) == SX_ERR_LIMIT);
    SX_CHECK(sx_roaring_serialize(&bitmap, bytes, sizeof(bytes), &size) == SX_OK);
    sx_roaring_view view;
    SX_CHECK(sx_roaring_view_open(&view, bytes, size) == SX_OK);
    SX_CHECK(sx_roaring_view_open(&view, bytes, size - 1) == SX_ERR_CORRUPT);
    uint8_t longer[257];
    memcpy(longer, bytes, size);
    longer[size] = 0;
    SX_CHECK(sx_roaring_view_open(&view, longer, size + 1) == SX_ERR_CORRUPT);

    /* Buckets out of order. */
    uint8_t swapped[256];
    memcpy(swapped, bytes, size);
    sx_roaring_clear(&bitmap);
    size_t half = (size - 8) / 2;
    memcpy(swapped + 8, bytes + 8 + half, half);
    memcpy(swapped + 8 + half, bytes + 8, half);
    SX_CHECK(sx_roaring_view_open(&view, swapped, size) == SX_ERR_CORRUPT);

    /* An unknown cookie. */
    memcpy(swapped, bytes, size);
    swapped[12] = 0x99;
    SX_CHECK(sx_roaring_view_open(&view, swapped, size) == SX_ERR_CORRUPT);
}

static const sx_test_case roaring_cases[] = {
    {"empty", test_empty},
    {"container_conversions", test_container_conversions},
    {"many_runs", test_many_runs},
    {"matches_croaring_bytes", test_matches_croaring_bytes},
    {"reads_croaring_bytes", test_reads_croaring_bytes},
    {"random_operations", test_random_operations},
    {"damaged_views_stay_in_bounds", test_damaged_views_stay_in_bounds},
    {"counted_allocators", test_counted_allocators},
    {"arena_allocator", test_arena_allocator},
    {"rejects_bad_structure", test_rejects_bad_structure},
};

const sx_test_suite sx_test_suite_roaring = SX_TEST_SUITE("roaring", roaring_cases);
