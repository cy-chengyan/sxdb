/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Compiler, language, and platform requirements, plus thin wrappers around compiler
 * extensions. Every other module uses these macros instead of raw __attribute__ or
 * __builtin_* spellings.
 */

#ifndef SX_COMMON_COMPILER_H
#define SX_COMMON_COMPILER_H

#if !defined(__GNUC__) && !defined(__clang__)
#error "sxdb requires GCC or Clang"
#endif

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201710L
#error "sxdb requires C17 or later"
#endif

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "sxdb supports little-endian platforms only"
#endif

#if defined(__x86_64__)
#define SX_ARCH_X86_64 1
#elif defined(__aarch64__)
#define SX_ARCH_ARM64 1
#else
#error "sxdb supports x86_64 and ARM64 only"
#endif

#define SX_LIKELY(x)   __builtin_expect(!!(x), 1)
#define SX_UNLIKELY(x) __builtin_expect(!!(x), 0)

#define SX_NOINLINE                  __attribute__((noinline))
#define SX_NODISCARD                 __attribute__((warn_unused_result))
#define SX_PRINTF_FORMAT(fmt, first) __attribute__((format(printf, fmt, first)))
#define SX_ALIGNED(bytes)            __attribute__((aligned(bytes)))

#define SX_UNUSED(x)     ((void)(x))
#define SX_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Number of values in one execution vector and in one full storage block. */
#define SX_VECTOR_SIZE 1024u

#endif /* SX_COMMON_COMPILER_H */
