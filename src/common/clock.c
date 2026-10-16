/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "common/clock.h"

#include <stdio.h>
#include <time.h>

uint64_t sx_clock_monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * (uint64_t)SX_NANOS_PER_SECOND + (uint64_t)now.tv_nsec;
}

int64_t sx_clock_realtime_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (int64_t)now.tv_sec * SX_NANOS_PER_SECOND + (int64_t)now.tv_nsec;
}

/* Splits a nanosecond timestamp into broken-down UTC time and the sub-second remainder. */
static void split_utc(int64_t unix_ns, struct tm* fields, int64_t* nanos_of_second) {
    int64_t seconds = unix_ns / SX_NANOS_PER_SECOND;
    int64_t remainder = unix_ns % SX_NANOS_PER_SECOND;
    if (remainder < 0) {
        remainder += SX_NANOS_PER_SECOND;
        seconds -= 1;
    }
    time_t as_time = (time_t)seconds;
    gmtime_r(&as_time, fields);
    *nanos_of_second = remainder;
}

void sx_format_utc_millis(int64_t unix_ns, char out[SX_TIMESTAMP_TEXT_SIZE]) {
    struct tm fields;
    int64_t nanos = 0;
    split_utc(unix_ns, &fields, &nanos);
    snprintf(out, SX_TIMESTAMP_TEXT_SIZE, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             fields.tm_year + 1900, fields.tm_mon + 1, fields.tm_mday, fields.tm_hour,
             fields.tm_min, fields.tm_sec, (int)(nanos / 1000000));
}

void sx_format_utc_compact(int64_t unix_ns, char out[SX_TIMESTAMP_TEXT_SIZE]) {
    struct tm fields;
    int64_t nanos = 0;
    split_utc(unix_ns, &fields, &nanos);
    snprintf(out, SX_TIMESTAMP_TEXT_SIZE, "%04d%02d%02dT%02d%02d%02dZ", fields.tm_year + 1900,
             fields.tm_mon + 1, fields.tm_mday, fields.tm_hour, fields.tm_min, fields.tm_sec);
}
