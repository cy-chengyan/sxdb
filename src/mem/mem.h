/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Memory manager. See docs/DESIGN.md section 6.
 *
 * Every significant memory consumer reserves its memory here, against one fixed budget
 * (memory_limit). Consumers are grouped into pools so usage can be reported per pool.
 *
 * When a reservation does not fit, the manager asks registered reclaimers (for example
 * the buffer pool) to give memory back, then retries. Only if that is not enough does
 * the reservation fail with SX_ERR_NOMEM. Memory managed by sxdb therefore never
 * exceeds the limit.
 *
 * Large allocations come straight from anonymous mmap in fixed size classes: 64 KB
 * frames and powers of two above that. Small objects come from arenas (mem/arena.h).
 */

#ifndef SX_MEM_MEM_H
#define SX_MEM_MEM_H

#include "common/error.h"

#include <stddef.h>
#include <stdint.h>

typedef enum sx_mem_pool {
    SX_MEM_POOL_BUFFER = 0, /* buffer pool (block cache); evictable */
    SX_MEM_POOL_MEMTABLE,   /* write buffers; released by flushing */
    SX_MEM_POOL_QUERY,      /* query working memory */
    SX_MEM_POOL_INDEX,      /* series index structures */
    SX_MEM_POOL_CONNECTION, /* network connection buffers */
    SX_MEM_POOL_OTHER,      /* everything else that is tracked */
    SX_MEM_POOL_COUNT,
} sx_mem_pool;

/* Smallest memory_limit accepted in a configuration. */
#define SX_MEM_MIN_LIMIT (UINT64_C(32) << 20)

/* Smallest allocation size class. */
#define SX_MEM_FRAME_SIZE ((size_t)64 << 10)

/* Maximum number of reclaimers that can be registered with one manager. */
#define SX_MEM_MAX_RECLAIMERS 8u

typedef struct sx_mem_manager sx_mem_manager;

/*
 * Gives memory back to the manager. Called when a reservation does not fit; `wanted` is
 * the shortfall in bytes. The reclaimer frees what it can through sx_mem_free or
 * sx_mem_unreserve and returns the number of bytes it released. A reclaimer must not
 * reserve memory itself.
 */
typedef uint64_t (*sx_mem_reclaim_fn)(void* context, uint64_t wanted);

typedef struct sx_mem_stats {
    uint64_t limit;
    uint64_t fixed_reserve;
    uint64_t used; /* fixed_reserve plus every pool */
    uint64_t peak_used;
    uint64_t pool_used[SX_MEM_POOL_COUNT];
    uint64_t failed_reservations;
} sx_mem_stats;

/* Memory available to this machine or container: min(physical memory, cgroup limit). */
uint64_t sx_mem_machine_limit(void);

/*
 * Validates a configured memory_limit: at least SX_MEM_MIN_LIMIT and, when
 * `machine_limit` is non-zero, at most `machine_limit`. Returns SX_ERR_INVALID_ARG for a
 * value below the minimum and SX_ERR_LIMIT for a value above what the machine has.
 */
sx_err sx_mem_check_limit(uint64_t limit, uint64_t machine_limit);

/*
 * Creates a manager. `fixed_reserve` covers untracked memory (thread stacks, code, small
 * objects) and is counted as used from the start, so it must be below `limit`.
 */
sx_err sx_mem_create(uint64_t limit, uint64_t fixed_reserve, sx_mem_manager** manager);

void sx_mem_destroy(sx_mem_manager* manager);

/*
 * Changes the limit at runtime. Lowering it asks reclaimers to release the excess
 * immediately. If usage still exceeds the new limit, it stays above until consumers free
 * memory; meanwhile new reservations fail.
 */
void sx_mem_set_limit(sx_mem_manager* manager, uint64_t limit);

sx_err sx_mem_register_reclaimer(sx_mem_manager* manager, sx_mem_reclaim_fn fn, void* context);

/* Reserves `bytes` for `pool`, reclaiming memory if needed. Returns SX_ERR_NOMEM on failure. */
sx_err sx_mem_reserve(sx_mem_manager* manager, sx_mem_pool pool, uint64_t bytes);

void sx_mem_unreserve(sx_mem_manager* manager, sx_mem_pool pool, uint64_t bytes);

/* The size actually allocated for a request of `size` bytes (0 if `size` is too large). */
size_t sx_mem_size_class(size_t size);

/*
 * Reserves and maps sx_mem_size_class(size) bytes of zeroed memory. Returns NULL if the
 * memory limit or the operating system does not allow it. Free with sx_mem_free using
 * the same pool and size.
 */
void* sx_mem_alloc(sx_mem_manager* manager, sx_mem_pool pool, size_t size);

void sx_mem_free(sx_mem_manager* manager, sx_mem_pool pool, void* memory, size_t size);

void sx_mem_get_stats(const sx_mem_manager* manager, sx_mem_stats* stats);

const char* sx_mem_pool_name(sx_mem_pool pool);

#endif /* SX_MEM_MEM_H */
