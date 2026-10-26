/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "common/allocator.h"

#include "common/compiler.h"

#include <stdlib.h>

static void* heap_allocate(void* context, size_t size) {
    SX_UNUSED(context);
    return malloc(size > 0 ? size : 1);
}

static void heap_release(void* context, void* memory, size_t size) {
    SX_UNUSED(context);
    SX_UNUSED(size);
    free(memory);
}

const sx_allocator sx_heap_allocator = {
    .allocate = heap_allocate,
    .release = heap_release,
    .context = NULL,
};
