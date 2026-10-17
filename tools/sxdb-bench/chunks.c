/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "chunks.h"

#include <stdlib.h>
#include <string.h>

bool bench_chunks_add(bench_chunks* chunks, uint32_t start, uint32_t count) {
    if (chunks->count == chunks->capacity) {
        uint32_t capacity = chunks->capacity == 0 ? 1024u : chunks->capacity * 2u;
        uint32_t* starts = realloc(chunks->starts, capacity * sizeof(uint32_t));
        if (starts == NULL) {
            return false;
        }
        chunks->starts = starts;
        uint32_t* counts = realloc(chunks->counts, capacity * sizeof(uint32_t));
        if (counts == NULL) {
            return false;
        }
        chunks->counts = counts;
        chunks->capacity = capacity;
    }
    chunks->starts[chunks->count] = start;
    chunks->counts[chunks->count] = count;
    chunks->count++;
    return true;
}

bool bench_chunks_fixed(bench_chunks* chunks, uint32_t total, uint32_t size) {
    for (uint32_t start = 0; start < total; start += size) {
        uint32_t remaining = total - start;
        if (!bench_chunks_add(chunks, start, remaining < size ? remaining : size)) {
            return false;
        }
    }
    return true;
}

void bench_chunks_free(bench_chunks* chunks) {
    free(chunks->starts);
    free(chunks->counts);
    memset(chunks, 0, sizeof(*chunks));
}
