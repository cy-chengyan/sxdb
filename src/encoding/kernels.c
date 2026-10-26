/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Chooses the codec kernel set. The SIMD sets are compiled only when the build enables
 * them (SX_SIMD_KERNELS, from the SXDB_SIMD CMake option) for the target architecture,
 * and used only when the CPU supports them.
 */

#include "encoding/kernels.h"

#include "common/compiler.h"
#include "os/os.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

static const char* const level_names[SX_SIMD_LEVEL_COUNT] = {
    [SX_SIMD_SCALAR] = "scalar",
    [SX_SIMD_AVX2] = "avx2",
    [SX_SIMD_AVX512] = "avx512",
    [SX_SIMD_NEON] = "neon",
};

/* The kernel set compiled for `level`, or NULL if this build has none. */
static const sx_codec_kernels* compiled_kernels(sx_simd_level level) {
    switch (level) {
        case SX_SIMD_SCALAR:
            return &sx_kernels_scalar;
#if defined(SX_SIMD_KERNELS) && defined(SX_ARCH_X86_64)
        case SX_SIMD_AVX2:
            return &sx_kernels_avx2;
        case SX_SIMD_AVX512:
            return &sx_kernels_avx512;
#endif
#if defined(SX_SIMD_KERNELS) && defined(SX_ARCH_ARM64)
        case SX_SIMD_NEON:
            return &sx_kernels_neon;
#endif
        default:
            return NULL;
    }
}

static bool cpu_supports(sx_simd_level level) {
    switch (level) {
        case SX_SIMD_SCALAR:
            return true;
        case SX_SIMD_AVX2:
            return sx_os_cpu_has_x86_64_v3();
        case SX_SIMD_AVX512:
            return sx_os_cpu_has_x86_64_v4();
        case SX_SIMD_NEON:
            /* NEON (Advanced SIMD) is a mandatory part of ARMv8-A. */
#if defined(SX_ARCH_ARM64)
            return true;
#else
            return false;
#endif
    }
    return false;
}

/*
 * The set in use. Codecs ask for it once per chunk, so after the first choice it is read
 * with a single load rather than through pthread_once, which costs a call and a
 * synchronizing check every time.
 */
static _Atomic(const sx_codec_kernels*) active_kernels;
static pthread_once_t select_once = PTHREAD_ONCE_INIT;

static void select_best(void) {
    static const sx_simd_level preference[] = {
        SX_SIMD_AVX512,
        SX_SIMD_AVX2,
        SX_SIMD_NEON,
        SX_SIMD_SCALAR,
    };
    for (size_t i = 0; i < SX_ARRAY_SIZE(preference); i++) {
        if (sx_simd_level_supported(preference[i])) {
            atomic_store_explicit(&active_kernels, compiled_kernels(preference[i]),
                                  memory_order_release);
            return;
        }
    }
}

const sx_codec_kernels* sx_codec_kernels_active(void) {
    const sx_codec_kernels* kernels = atomic_load_explicit(&active_kernels, memory_order_acquire);
    if (SX_LIKELY(kernels != NULL)) {
        return kernels;
    }
    pthread_once(&select_once, select_best);
    return atomic_load_explicit(&active_kernels, memory_order_acquire);
}

bool sx_simd_level_supported(sx_simd_level level) {
    if ((unsigned)level >= SX_SIMD_LEVEL_COUNT) {
        return false;
    }
    return compiled_kernels(level) != NULL && cpu_supports(level);
}

bool sx_simd_set_level(sx_simd_level level) {
    if (!sx_simd_level_supported(level)) {
        return false;
    }
    /* Run the default choice first, so that it cannot overwrite this one later. */
    pthread_once(&select_once, select_best);
    atomic_store_explicit(&active_kernels, compiled_kernels(level), memory_order_release);
    return true;
}

const char* sx_simd_level_name(sx_simd_level level) {
    if ((unsigned)level >= SX_SIMD_LEVEL_COUNT) {
        return "unknown";
    }
    return level_names[level];
}

bool sx_simd_level_from_name(const char* name, sx_simd_level* level) {
    for (unsigned i = 0; i < SX_SIMD_LEVEL_COUNT; i++) {
        if (strcmp(name, level_names[i]) == 0) {
            *level = (sx_simd_level)i;
            return true;
        }
    }
    return false;
}
