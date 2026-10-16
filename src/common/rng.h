/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * A small, fast, deterministic pseudo-random generator (SplitMix64) for sampling,
 * tests, and benchmarks. Not suitable for anything security related.
 */

#ifndef SX_COMMON_RNG_H
#define SX_COMMON_RNG_H

#include <stdint.h>

typedef struct sx_rng {
    uint64_t state;
} sx_rng;

static inline void sx_rng_seed(sx_rng* rng, uint64_t seed) {
    rng->state = seed;
}

static inline uint64_t sx_rng_next(sx_rng* rng) {
    rng->state += UINT64_C(0x9E3779B97F4A7C15);
    uint64_t z = rng->state;
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* Uniform double in [0, 1). */
static inline double sx_rng_next_double(sx_rng* rng) {
    return (double)(sx_rng_next(rng) >> 11) * 0x1.0p-53;
}

/* Value in [0, bound). The tiny modulo bias is irrelevant for the intended uses. */
static inline uint64_t sx_rng_below(sx_rng* rng, uint64_t bound) {
    return sx_rng_next(rng) % bound;
}

#endif /* SX_COMMON_RNG_H */
