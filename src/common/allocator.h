/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Allocator interface for data structures in common/, which cannot use the memory manager
 * directly because mem/ is a layer above. The owner of a structure passes the allocator:
 * mem/ provides ones over an arena and over the C heap with every byte counted against
 * memory_limit (mem/arena.h, mem/mem.h); sx_heap_allocator is the plain C heap, for tests
 * and tools.
 *
 * Allocations are aligned to at least 16 bytes. `release` gets the size that was
 * allocated, so an allocator can keep exact accounts without headers.
 */

#ifndef SX_COMMON_ALLOCATOR_H
#define SX_COMMON_ALLOCATOR_H

#include <stddef.h>

typedef struct sx_allocator {
    void* (*allocate)(void* context, size_t size); /* NULL on failure */
    void (*release)(void* context, void* memory, size_t size);
    void* context;
} sx_allocator;

static inline void* sx_allocate(const sx_allocator* allocator, size_t size) {
    return allocator->allocate(allocator->context, size);
}

/* Releases `memory` of `size` bytes; NULL is ignored. */
static inline void sx_release(const sx_allocator* allocator, void* memory, size_t size) {
    if (memory != NULL) {
        allocator->release(allocator->context, memory, size);
    }
}

/* The C heap, without accounting. */
extern const sx_allocator sx_heap_allocator;

#endif /* SX_COMMON_ALLOCATOR_H */
