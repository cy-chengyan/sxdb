/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Arena (bump) allocator for small objects with a shared lifetime, such as everything a
 * query allocates. Allocation is a pointer bump; there is no per-object free. All memory
 * is released at once by sx_arena_reset or sx_arena_destroy, so there is no
 * fragmentation and nothing can leak individually.
 *
 * Chunks come from the memory manager and are charged to the arena's pool.
 */

#ifndef SX_MEM_ARENA_H
#define SX_MEM_ARENA_H

#include "mem/mem.h"

#include <stddef.h>
#include <stdint.h>

typedef struct sx_arena sx_arena;

/* Creates an empty arena; no chunk is reserved until the first allocation. */
sx_arena* sx_arena_create(sx_mem_manager* manager, sx_mem_pool pool);

/* Releases every chunk and the arena itself. */
void sx_arena_destroy(sx_arena* arena);

/*
 * Allocates `size` bytes aligned to `alignment` (a power of two, at most 4096).
 * Returns NULL if the memory limit does not allow another chunk.
 */
void* sx_arena_alloc(sx_arena* arena, size_t size, size_t alignment);

/* Releases all allocations, keeping the first chunk for reuse. */
void sx_arena_reset(sx_arena* arena);

/* Bytes currently reserved from the memory manager by this arena. */
uint64_t sx_arena_reserved_bytes(const sx_arena* arena);

#endif /* SX_MEM_ARENA_H */
