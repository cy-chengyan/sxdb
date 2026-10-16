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

/*
 * Calendar fields of a UTC time. Each field is reduced to its printable range, so the
 * formatted text has a fixed length; nanosecond timestamps only span the years 1677 to
 * 2262, so the reduction never changes a real value.
 */
typedef struct utc_fields {
    unsigned year;
    unsigned month;
    unsigned day;
    unsigned hour;
    unsigned minute;
    unsigned second;
    unsigned millisecond;
} utc_fields;

static utc_fields split_utc(int64_t unix_ns) {
    int64_t seconds = unix_ns / SX_NANOS_PER_SECOND;
    int64_t remainder = unix_ns % SX_NANOS_PER_SECOND;
    if (remainder < 0) {
        remainder += SX_NANOS_PER_SECOND;
        seconds -= 1;
    }
    time_t as_time = (time_t)seconds;
    struct tm broken_down;
    gmtime_r(&as_time, &broken_down);

    utc_fields fields = {
        .year = (unsigned)(broken_down.tm_year + 1900) % 10000u,
        .month = (unsigned)(broken_down.tm_mon + 1) % 100u,
        .day = (unsigned)broken_down.tm_mday % 100u,
        .hour = (unsigned)broken_down.tm_hour % 100u,
        .minute = (unsigned)broken_down.tm_min % 100u,
        .second = (unsigned)broken_down.tm_sec % 100u,
        .millisecond = (unsigned)(remainder / 1000000) % 1000u,
    };
    return fields;
}

void sx_format_utc_millis(int64_t unix_ns, char out[SX_TIMESTAMP_TEXT_SIZE]) {
    utc_fields fields = split_utc(unix_ns);
    snprintf(out, SX_TIMESTAMP_TEXT_SIZE, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", fields.year,
             fields.month, fields.day, fields.hour, fields.minute, fields.second,
             fields.millisecond);
}

void sx_format_utc_compact(int64_t unix_ns, char out[SX_TIMESTAMP_TEXT_SIZE]) {
    utc_fields fields = split_utc(unix_ns);
    snprintf(out, SX_TIMESTAMP_TEXT_SIZE, "%04u%02u%02uT%02u%02u%02uZ", fields.year, fields.month,
             fields.day, fields.hour, fields.minute, fields.second);
}
