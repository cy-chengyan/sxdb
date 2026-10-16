/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sx_test.h"

#include "common/compiler.h"
#include "mem/arena.h"
#include "mem/mem.h"

#include <pthread.h>

#define MIB (UINT64_C(1) << 20)

static void test_check_limit(void) {
    SX_CHECK(sx_mem_check_limit(SX_MEM_MIN_LIMIT - 1, 0) == SX_ERR_INVALID_ARG);
    SX_CHECK(sx_mem_check_limit(SX_MEM_MIN_LIMIT, 0) == SX_OK);
    SX_CHECK(sx_mem_check_limit(64 * MIB, 32 * MIB) == SX_ERR_LIMIT);
    SX_CHECK(sx_mem_check_limit(64 * MIB, 128 * MIB) == SX_OK);
    SX_CHECK(sx_mem_machine_limit() > 0);
}

static void test_size_classes(void) {
    SX_CHECK_EQ_U64(sx_mem_size_class(1), SX_MEM_FRAME_SIZE);
    SX_CHECK_EQ_U64(sx_mem_size_class(SX_MEM_FRAME_SIZE), SX_MEM_FRAME_SIZE);
    SX_CHECK_EQ_U64(sx_mem_size_class(SX_MEM_FRAME_SIZE + 1), 2 * SX_MEM_FRAME_SIZE);
    SX_CHECK_EQ_U64(sx_mem_size_class(MIB), MIB);
    SX_CHECK_EQ_U64(sx_mem_size_class(MIB + 1), 2 * MIB);
}

static void test_reserve_respects_limit(void) {
    sx_mem_manager* manager = NULL;
    SX_CHECK(sx_mem_create(4 * MIB, 1 * MIB, &manager) == SX_OK);

    SX_CHECK(sx_mem_reserve(manager, SX_MEM_POOL_QUERY, 2 * MIB) == SX_OK);
    SX_CHECK(sx_mem_reserve(manager, SX_MEM_POOL_MEMTABLE, 1 * MIB) == SX_OK);
    SX_CHECK(sx_mem_reserve(manager, SX_MEM_POOL_QUERY, 1) == SX_ERR_NOMEM);

    sx_mem_stats stats;
    sx_mem_get_stats(manager, &stats);
    SX_CHECK_EQ_U64(stats.used, 4 * MIB);
    SX_CHECK_EQ_U64(stats.pool_used[SX_MEM_POOL_QUERY], 2 * MIB);
    SX_CHECK_EQ_U64(stats.pool_used[SX_MEM_POOL_MEMTABLE], 1 * MIB);
    SX_CHECK_EQ_U64(stats.failed_reservations, 1);

    sx_mem_unreserve(manager, SX_MEM_POOL_QUERY, 2 * MIB);
    sx_mem_unreserve(manager, SX_MEM_POOL_MEMTABLE, 1 * MIB);
    sx_mem_get_stats(manager, &stats);
    SX_CHECK_EQ_U64(stats.used, 1 * MIB);
    SX_CHECK_EQ_U64(stats.peak_used, 4 * MIB);
    sx_mem_destroy(manager);
}

/* A fake cache that holds frames and gives them back when the manager asks. */
typedef struct fake_cache {
    sx_mem_manager* manager;
    void* frames[64];
    size_t count;
    uint64_t reclaim_calls;
} fake_cache;

static uint64_t fake_cache_reclaim(void* context, uint64_t wanted) {
    fake_cache* cache = context;
    uint64_t released = 0;
    cache->reclaim_calls++;
    while (released < wanted && cache->count > 0) {
        cache->count--;
        sx_mem_free(cache->manager, SX_MEM_POOL_BUFFER, cache->frames[cache->count],
                    SX_MEM_FRAME_SIZE);
        released += SX_MEM_FRAME_SIZE;
    }
    return released;
}

static void test_reclaimer_frees_cache_for_new_reservations(void) {
    sx_mem_manager* manager = NULL;
    SX_CHECK(sx_mem_create(1 * MIB, 0, &manager) == SX_OK);
    fake_cache cache = {.manager = manager};
    SX_CHECK(sx_mem_register_reclaimer(manager, fake_cache_reclaim, &cache) == SX_OK);

    /* Fill the whole budget with cache frames. */
    while (cache.count < 16) {
        void* frame = sx_mem_alloc(manager, SX_MEM_POOL_BUFFER, SX_MEM_FRAME_SIZE);
        SX_CHECK(frame != NULL);
        cache.frames[cache.count] = frame;
        cache.count++;
    }
    SX_CHECK_EQ_U64(cache.reclaim_calls, 0);

    /* A query reservation succeeds by evicting cache frames. */
    size_t frames_before = cache.count;
    SX_CHECK(sx_mem_reserve(manager, SX_MEM_POOL_QUERY, 3 * SX_MEM_FRAME_SIZE) == SX_OK);
    SX_CHECK(cache.count <= frames_before - 3);

    sx_mem_stats stats;
    sx_mem_get_stats(manager, &stats);
    SX_CHECK(stats.used <= stats.limit);

    sx_mem_unreserve(manager, SX_MEM_POOL_QUERY, 3 * SX_MEM_FRAME_SIZE);
    fake_cache_reclaim(&cache, UINT64_MAX);
    sx_mem_get_stats(manager, &stats);
    SX_CHECK_EQ_U64(stats.used, 0);
    sx_mem_destroy(manager);
}

static void test_lowering_limit_reclaims(void) {
    sx_mem_manager* manager = NULL;
    SX_CHECK(sx_mem_create(2 * MIB, 0, &manager) == SX_OK);
    fake_cache cache = {.manager = manager};
    SX_CHECK(sx_mem_register_reclaimer(manager, fake_cache_reclaim, &cache) == SX_OK);
    for (size_t i = 0; i < 20; i++) {
        cache.frames[cache.count] = sx_mem_alloc(manager, SX_MEM_POOL_BUFFER, SX_MEM_FRAME_SIZE);
        SX_CHECK(cache.frames[cache.count] != NULL);
        cache.count++;
    }

    sx_mem_set_limit(manager, 512 * 1024);
    sx_mem_stats stats;
    sx_mem_get_stats(manager, &stats);
    SX_CHECK_EQ_U64(stats.limit, 512 * 1024);
    SX_CHECK(stats.used <= 512 * 1024);

    fake_cache_reclaim(&cache, UINT64_MAX);
    sx_mem_destroy(manager);
}

static void test_create_rejects_reserve_above_limit(void) {
    sx_mem_manager* manager = NULL;
    SX_CHECK(sx_mem_create(MIB, MIB, &manager) == SX_ERR_INVALID_ARG);
}

typedef struct reserve_worker_args {
    sx_mem_manager* manager;
    int iterations;
} reserve_worker_args;

static void* reserve_worker(void* arg) {
    reserve_worker_args* args = arg;
    for (int i = 0; i < args->iterations; i++) {
        if (sx_mem_reserve(args->manager, SX_MEM_POOL_QUERY, 4096) == SX_OK) {
            sx_mem_unreserve(args->manager, SX_MEM_POOL_QUERY, 4096);
        }
    }
    return NULL;
}

static void test_concurrent_reservations_balance(void) {
    sx_mem_manager* manager = NULL;
    SX_CHECK(sx_mem_create(64 * 1024, 0, &manager) == SX_OK);
    pthread_t threads[4];
    reserve_worker_args args = {.manager = manager, .iterations = 20000};
    for (size_t i = 0; i < SX_ARRAY_SIZE(threads); i++) {
        SX_CHECK(pthread_create(&threads[i], NULL, reserve_worker, &args) == 0);
    }
    for (size_t i = 0; i < SX_ARRAY_SIZE(threads); i++) {
        pthread_join(threads[i], NULL);
    }
    sx_mem_stats stats;
    sx_mem_get_stats(manager, &stats);
    SX_CHECK_EQ_U64(stats.used, 0);
    SX_CHECK(stats.peak_used <= 64 * 1024);
    sx_mem_destroy(manager);
}

static void test_arena_allocations(void) {
    sx_mem_manager* manager = NULL;
    SX_CHECK(sx_mem_create(16 * MIB, 0, &manager) == SX_OK);
    sx_arena* arena = sx_arena_create(manager, SX_MEM_POOL_QUERY);
    SX_CHECK(arena != NULL);

    uint8_t* previous = NULL;
    for (int i = 0; i < 10000; i++) {
        uint8_t* p = sx_arena_alloc(arena, 24, 8);
        SX_CHECK(p != NULL);
        SX_CHECK(((uintptr_t)p & 7u) == 0);
        SX_CHECK(p != previous);
        memset(p, 0xAB, 24);
        previous = p;
    }
    void* aligned = sx_arena_alloc(arena, 100, 4096);
    SX_CHECK(aligned != NULL);
    SX_CHECK(((uintptr_t)aligned & 4095u) == 0);

    void* large = sx_arena_alloc(arena, 3 * MIB, 16);
    SX_CHECK(large != NULL);
    memset(large, 0, 3 * MIB);

    sx_mem_stats stats;
    sx_mem_get_stats(manager, &stats);
    SX_CHECK_EQ_U64(stats.pool_used[SX_MEM_POOL_QUERY], sx_arena_reserved_bytes(arena));
    SX_CHECK(sx_arena_reserved_bytes(arena) >= 3 * MIB);

    sx_arena_reset(arena);
    SX_CHECK_EQ_U64(sx_arena_reserved_bytes(arena), SX_MEM_FRAME_SIZE);
    SX_CHECK(sx_arena_alloc(arena, 64, 8) != NULL);
    SX_CHECK(sx_arena_alloc(arena, 64, 3) == NULL);

    sx_arena_destroy(arena);
    sx_mem_get_stats(manager, &stats);
    SX_CHECK_EQ_U64(stats.used, 0);
    sx_mem_destroy(manager);
}

static void test_arena_fails_cleanly_at_limit(void) {
    sx_mem_manager* manager = NULL;
    SX_CHECK(sx_mem_create(2 * SX_MEM_FRAME_SIZE, 0, &manager) == SX_OK);
    sx_arena* arena = sx_arena_create(manager, SX_MEM_POOL_QUERY);
    SX_CHECK(arena != NULL);
    SX_CHECK(sx_arena_alloc(arena, 1000, 8) != NULL);
    SX_CHECK(sx_arena_alloc(arena, 4 * SX_MEM_FRAME_SIZE, 8) == NULL);
    SX_CHECK(sx_arena_alloc(arena, 1000, 8) != NULL);
    sx_arena_destroy(arena);
    sx_mem_destroy(manager);
}

static void test_pool_names(void) {
    SX_CHECK_EQ_STR(sx_mem_pool_name(SX_MEM_POOL_BUFFER), "buffer_pool");
    SX_CHECK_EQ_STR(sx_mem_pool_name(SX_MEM_POOL_MEMTABLE), "memtable");
}

static const sx_test_case cases[] = {
    {"check_limit", test_check_limit},
    {"size_classes", test_size_classes},
    {"reserve_respects_limit", test_reserve_respects_limit},
    {"reclaimer_frees_cache", test_reclaimer_frees_cache_for_new_reservations},
    {"lowering_limit_reclaims", test_lowering_limit_reclaims},
    {"create_rejects_bad_reserve", test_create_rejects_reserve_above_limit},
    {"concurrent_reservations", test_concurrent_reservations_balance},
    {"arena_allocations", test_arena_allocations},
    {"arena_fails_at_limit", test_arena_fails_cleanly_at_limit},
    {"pool_names", test_pool_names},
};

const sx_test_suite sx_test_suite_mem = SX_TEST_SUITE("mem", cases);
