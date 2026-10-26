/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mem/mem.h"

#include "common/bits.h"
#include "os/os.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct reclaimer {
    sx_mem_reclaim_fn fn;
    void* context;
} reclaimer;

struct sx_mem_manager {
    _Atomic uint64_t limit;
    uint64_t fixed_reserve;
    _Atomic uint64_t used;
    _Atomic uint64_t peak_used;
    _Atomic uint64_t pool_used[SX_MEM_POOL_COUNT];
    _Atomic uint64_t failed_reservations;

    /* Serializes reclaim attempts and protects the reclaimer list. */
    pthread_mutex_t reclaim_mutex;
    reclaimer reclaimers[SX_MEM_MAX_RECLAIMERS];
    size_t reclaimer_count;
};

uint64_t sx_mem_machine_limit(void) {
    uint64_t physical = sx_os_physical_memory();
    uint64_t container = sx_os_container_memory_limit();
    if (container != 0 && (physical == 0 || container < physical)) {
        return container;
    }
    return physical;
}

sx_err sx_mem_check_limit(uint64_t limit, uint64_t machine_limit) {
    if (limit < SX_MEM_MIN_LIMIT) {
        return SX_ERR_INVALID_ARG;
    }
    if (machine_limit != 0 && limit > machine_limit) {
        return SX_ERR_LIMIT;
    }
    return SX_OK;
}

sx_err sx_mem_create(uint64_t limit, uint64_t fixed_reserve, sx_mem_manager** manager) {
    if (fixed_reserve >= limit) {
        return SX_ERR_INVALID_ARG;
    }
    sx_mem_manager* created = calloc(1, sizeof(*created));
    if (created == NULL) {
        return SX_ERR_NOMEM;
    }
    if (pthread_mutex_init(&created->reclaim_mutex, NULL) != 0) {
        free(created);
        return SX_ERR_NOMEM;
    }
    atomic_init(&created->limit, limit);
    created->fixed_reserve = fixed_reserve;
    atomic_init(&created->used, fixed_reserve);
    atomic_init(&created->peak_used, fixed_reserve);
    for (size_t i = 0; i < SX_MEM_POOL_COUNT; i++) {
        atomic_init(&created->pool_used[i], 0);
    }
    atomic_init(&created->failed_reservations, 0);
    *manager = created;
    return SX_OK;
}

void sx_mem_destroy(sx_mem_manager* manager) {
    if (manager == NULL) {
        return;
    }
    pthread_mutex_destroy(&manager->reclaim_mutex);
    free(manager);
}

sx_err sx_mem_register_reclaimer(sx_mem_manager* manager, sx_mem_reclaim_fn fn, void* context) {
    sx_err err = SX_OK;
    pthread_mutex_lock(&manager->reclaim_mutex);
    if (manager->reclaimer_count == SX_MEM_MAX_RECLAIMERS) {
        err = SX_ERR_LIMIT;
    } else {
        manager->reclaimers[manager->reclaimer_count].fn = fn;
        manager->reclaimers[manager->reclaimer_count].context = context;
        manager->reclaimer_count++;
    }
    pthread_mutex_unlock(&manager->reclaim_mutex);
    return err;
}

static void update_peak(sx_mem_manager* manager, uint64_t used) {
    uint64_t peak = atomic_load_explicit(&manager->peak_used, memory_order_relaxed);
    while (used > peak) {
        if (atomic_compare_exchange_weak(&manager->peak_used, &peak, used)) {
            break;
        }
    }
}

/* Reserves without reclaiming. Returns false if the reservation would exceed the limit. */
static bool try_reserve(sx_mem_manager* manager, sx_mem_pool pool, uint64_t bytes) {
    uint64_t limit = atomic_load_explicit(&manager->limit, memory_order_relaxed);
    uint64_t used = atomic_load_explicit(&manager->used, memory_order_relaxed);
    for (;;) {
        if (bytes > limit || used > limit - bytes) {
            return false;
        }
        if (atomic_compare_exchange_weak(&manager->used, &used, used + bytes)) {
            break;
        }
    }
    atomic_fetch_add_explicit(&manager->pool_used[pool], bytes, memory_order_relaxed);
    update_peak(manager, used + bytes);
    return true;
}

/* Bytes by which usage plus `extra` exceeds the limit (0 if it fits). */
static uint64_t shortfall(const sx_mem_manager* manager, uint64_t extra) {
    uint64_t limit = atomic_load(&manager->limit);
    uint64_t used = atomic_load(&manager->used);
    if (used + extra <= limit) {
        return 0;
    }
    return used + extra - limit;
}

/*
 * Asks reclaimers, in registration order, to release memory until `extra` more bytes
 * fit. Called with reclaim_mutex held.
 */
static void reclaim_locked(sx_mem_manager* manager, uint64_t extra) {
    for (size_t i = 0; i < manager->reclaimer_count; i++) {
        uint64_t wanted = shortfall(manager, extra);
        if (wanted == 0) {
            return;
        }
        manager->reclaimers[i].fn(manager->reclaimers[i].context, wanted);
    }
}

sx_err sx_mem_reserve(sx_mem_manager* manager, sx_mem_pool pool, uint64_t bytes) {
    if (try_reserve(manager, pool, bytes)) {
        return SX_OK;
    }

    pthread_mutex_lock(&manager->reclaim_mutex);
    bool reserved = try_reserve(manager, pool, bytes);
    if (!reserved) {
        reclaim_locked(manager, bytes);
        reserved = try_reserve(manager, pool, bytes);
    }
    pthread_mutex_unlock(&manager->reclaim_mutex);

    if (!reserved) {
        atomic_fetch_add_explicit(&manager->failed_reservations, 1, memory_order_relaxed);
        return SX_ERR_NOMEM;
    }
    return SX_OK;
}

void sx_mem_unreserve(sx_mem_manager* manager, sx_mem_pool pool, uint64_t bytes) {
    atomic_fetch_sub_explicit(&manager->pool_used[pool], bytes, memory_order_relaxed);
    atomic_fetch_sub_explicit(&manager->used, bytes, memory_order_relaxed);
}

void sx_mem_set_limit(sx_mem_manager* manager, uint64_t limit) {
    atomic_store(&manager->limit, limit);
    pthread_mutex_lock(&manager->reclaim_mutex);
    reclaim_locked(manager, 0);
    pthread_mutex_unlock(&manager->reclaim_mutex);
}

size_t sx_mem_size_class(size_t size) {
    if (size <= SX_MEM_FRAME_SIZE) {
        return SX_MEM_FRAME_SIZE;
    }
    if (size > ((size_t)1 << 62)) {
        return 0;
    }
    uint32_t width = sx_bit_width_u64((uint64_t)size - 1u);
    return (size_t)1 << width;
}

void* sx_mem_alloc(sx_mem_manager* manager, sx_mem_pool pool, size_t size) {
    size_t class_size = sx_mem_size_class(size);
    if (class_size == 0) {
        return NULL;
    }
    if (sx_mem_reserve(manager, pool, class_size) != SX_OK) {
        return NULL;
    }
    void* memory = sx_os_map_anonymous(class_size);
    if (memory == NULL) {
        sx_mem_unreserve(manager, pool, class_size);
        return NULL;
    }
    return memory;
}

void sx_mem_free(sx_mem_manager* manager, sx_mem_pool pool, void* memory, size_t size) {
    if (memory == NULL) {
        return;
    }
    size_t class_size = sx_mem_size_class(size);
    sx_os_unmap(memory, class_size);
    sx_mem_unreserve(manager, pool, class_size);
}

void sx_mem_get_stats(const sx_mem_manager* manager, sx_mem_stats* stats) {
    stats->limit = atomic_load(&manager->limit);
    stats->fixed_reserve = manager->fixed_reserve;
    stats->used = atomic_load(&manager->used);
    stats->peak_used = atomic_load(&manager->peak_used);
    for (size_t i = 0; i < SX_MEM_POOL_COUNT; i++) {
        stats->pool_used[i] = atomic_load(&manager->pool_used[i]);
    }
    stats->failed_reservations = atomic_load(&manager->failed_reservations);
}

const char* sx_mem_pool_name(sx_mem_pool pool) {
    switch (pool) {
        case SX_MEM_POOL_BUFFER:
            return "buffer_pool";
        case SX_MEM_POOL_MEMTABLE:
            return "memtable";
        case SX_MEM_POOL_QUERY:
            return "query";
        case SX_MEM_POOL_INDEX:
            return "index";
        case SX_MEM_POOL_CONNECTION:
            return "connection";
        case SX_MEM_POOL_OTHER:
            return "other";
        case SX_MEM_POOL_COUNT:
            break;
    }
    return "unknown";
}

/*
 * The C heap with accounting. The allocator's own bookkeeping per allocation is not
 * counted; it is small next to the containers and buffers allocated this way.
 */
static void* heap_allocate(void* context, size_t size) {
    sx_mem_heap* heap = context;
    if (sx_mem_reserve(heap->manager, heap->pool, size) != SX_OK) {
        return NULL;
    }
    void* memory = malloc(size > 0 ? size : 1);
    if (memory == NULL) {
        sx_mem_unreserve(heap->manager, heap->pool, size);
    }
    return memory;
}

static void heap_release(void* context, void* memory, size_t size) {
    sx_mem_heap* heap = context;
    free(memory);
    sx_mem_unreserve(heap->manager, heap->pool, size);
}

sx_allocator sx_mem_heap_allocator(sx_mem_heap* heap) {
    sx_allocator allocator = {
        .allocate = heap_allocate,
        .release = heap_release,
        .context = heap,
    };
    return allocator;
}
