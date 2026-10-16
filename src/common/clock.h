/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Clocks and UTC time formatting.
 */

#ifndef SX_COMMON_CLOCK_H
#define SX_COMMON_CLOCK_H

#include <stddef.h>
#include <stdint.h>

#define SX_NANOS_PER_SECOND INT64_C(1000000000)

/* Monotonic time in nanoseconds, for measuring intervals. */
uint64_t sx_clock_monotonic_ns(void);

/* Wall-clock time in nanoseconds since the Unix epoch (UTC). */
int64_t sx_clock_realtime_ns(void);

/* Size of a buffer that fits any timestamp produced by the formatting functions below. */
#define SX_TIMESTAMP_TEXT_SIZE 32u

/* Formats as "2026-10-16T08:00:00.123Z" (UTC, millisecond precision). */
void sx_format_utc_millis(int64_t unix_ns, char out[SX_TIMESTAMP_TEXT_SIZE]);

/* Formats as "20261016T080000Z" (UTC, second precision), suitable for file names. */
void sx_format_utc_compact(int64_t unix_ns, char out[SX_TIMESTAMP_TEXT_SIZE]);

#endif /* SX_COMMON_CLOCK_H */
