/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mem/arena.h"

#include "common/bits.h"

#include <stdbool.h>
#include <stdlib.h>

#define MAX_ALIGNMENT 4096u

/* Header at the start of every chunk; the usable space follows it. */
typedef struct arena_chunk {
    struct arena_chunk* next;
    size_t size; /* total mapped size, including this header */
} arena_chunk;

struct sx_arena {
    sx_mem_manager* manager;
    sx_mem_pool pool;
    arena_chunk* first;   /* oldest chunk; kept by sx_arena_reset */
    arena_chunk* current; /* chunk allocations are taken from */
    size_t offset;        /* next free byte within `current` */
    uint64_t reserved;
};

static size_t chunk_header_size(void) {
    return sx_align_up(sizeof(arena_chunk), 16);
}

sx_arena* sx_arena_create(sx_mem_manager* manager, sx_mem_pool pool) {
    sx_arena* arena = calloc(1, sizeof(*arena));
    if (arena == NULL) {
        return NULL;
    }
    arena->manager = manager;
    arena->pool = pool;
    return arena;
}

static void free_chunk(sx_arena* arena, arena_chunk* chunk) {
    arena->reserved -= chunk->size;
    sx_mem_free(arena->manager, arena->pool, chunk, chunk->size);
}

/* Frees every chunk after `keep` in the list (all chunks if `keep` is NULL). */
static void free_chunks_after(sx_arena* arena, arena_chunk* keep) {
    arena_chunk* chunk = arena->current;
    while (chunk != NULL && chunk != keep) {
        arena_chunk* next = chunk->next;
        free_chunk(arena, chunk);
        chunk = next;
    }
}

void sx_arena_destroy(sx_arena* arena) {
    if (arena == NULL) {
        return;
    }
    free_chunks_after(arena, NULL);
    free(arena);
}

void sx_arena_reset(sx_arena* arena) {
    if (arena->first == NULL) {
        return;
    }
    free_chunks_after(arena, arena->first);
    arena->first->next = NULL;
    arena->current = arena->first;
    arena->offset = chunk_header_size();
}

/* Adds a chunk large enough for `size` bytes at `alignment` and makes it current. */
static bool add_chunk(sx_arena* arena, size_t size, size_t alignment) {
    size_t needed = chunk_header_size() + size + alignment;
    size_t chunk_size = sx_mem_size_class(needed);
    if (chunk_size == 0) {
        return false;
    }
    arena_chunk* chunk = sx_mem_alloc(arena->manager, arena->pool, chunk_size);
    if (chunk == NULL) {
        return false;
    }
    chunk->next = arena->current;
    chunk->size = chunk_size;
    arena->current = chunk;
    arena->offset = chunk_header_size();
    arena->reserved += chunk_size;
    if (arena->first == NULL) {
        arena->first = chunk;
    }
    return true;
}

void* sx_arena_alloc(sx_arena* arena, size_t size, size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0 || alignment > MAX_ALIGNMENT) {
        return NULL;
    }
    if (arena->current != NULL) {
        size_t start = sx_align_up(arena->offset, alignment);
        if (start <= arena->current->size && size <= arena->current->size - start) {
            arena->offset = start + size;
            return (uint8_t*)arena->current + start;
        }
    }
    if (!add_chunk(arena, size, alignment)) {
        return NULL;
    }
    size_t start = sx_align_up(arena->offset, alignment);
    arena->offset = start + size;
    return (uint8_t*)arena->current + start;
}

uint64_t sx_arena_reserved_bytes(const sx_arena* arena) {
    return arena->reserved;
}
