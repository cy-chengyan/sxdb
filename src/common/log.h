/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Logging. See docs/DESIGN.md section 12.3.
 *
 * Each event is one plain-text line:
 *     2026-10-16T08:00:00.123Z WARN  [wal] fsync took 1532 ms, group commit delayed
 *
 * Before sx_log_open() is called, WARN and ERROR messages go straight to stderr. After
 * it, callers only format the line and push it onto a queue; a background thread writes
 * the queue to stderr or to rotating files. When the queue is full, INFO and DEBUG lines
 * are dropped (and counted) while WARN and ERROR lines wait for space, so logging never
 * blocks callers except in that rare case.
 */

#ifndef SX_COMMON_LOG_H
#define SX_COMMON_LOG_H

#include "common/compiler.h"
#include "common/error.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum sx_log_level {
    SX_LOG_LEVEL_ERROR = 0,
    SX_LOG_LEVEL_WARN = 1,
    SX_LOG_LEVEL_INFO = 2,
    SX_LOG_LEVEL_DEBUG = 3,
} sx_log_level;

typedef enum sx_log_output {
    SX_LOG_OUTPUT_STDERR,
    SX_LOG_OUTPUT_FILE,
} sx_log_output;

typedef struct sx_log_config {
    sx_log_level level;
    sx_log_output output;
    /* The settings below apply to SX_LOG_OUTPUT_FILE only. */
    const char* dir;     /* directory for log files */
    const char* name;    /* file name stem: "<name>.log", rotated to "<name>-<UTC time>.log" */
    uint64_t file_size;  /* rotate once the current file would exceed this many bytes */
    uint64_t total_size; /* keep all files of this log together within this many bytes */
} sx_log_config;

/* Starts asynchronous logging. Returns SX_ERR_INVALID_ARG for a bad configuration. */
sx_err sx_log_open(const sx_log_config* config);

/* Writes every queued line, stops the background thread, and returns to stderr output. */
void sx_log_close(void);

void sx_log_set_level(sx_log_level level);

bool sx_log_enabled(sx_log_level level);

/* Formats and logs one event. Prefer the SX_LOG_* macros, which skip disabled levels. */
void sx_log_write(sx_log_level level, const char* module, const char* format, ...)
    SX_PRINTF_FORMAT(3, 4);

/* Number of INFO and DEBUG lines dropped because the queue was full. */
uint64_t sx_log_dropped_count(void);

/* Parses "error", "warn", "info", or "debug". */
sx_err sx_log_parse_level(const char* text, sx_log_level* level);

const char* sx_log_level_name(sx_log_level level);

#define SX_LOG(level, module, ...)                                                                 \
    do {                                                                                           \
        if (sx_log_enabled(level)) {                                                               \
            sx_log_write((level), (module), __VA_ARGS__);                                          \
        }                                                                                          \
    } while (0)

#define SX_LOG_ERROR(module, ...) SX_LOG(SX_LOG_LEVEL_ERROR, module, __VA_ARGS__)
#define SX_LOG_WARN(module, ...)  SX_LOG(SX_LOG_LEVEL_WARN, module, __VA_ARGS__)
#define SX_LOG_INFO(module, ...)  SX_LOG(SX_LOG_LEVEL_INFO, module, __VA_ARGS__)
#define SX_LOG_DEBUG(module, ...) SX_LOG(SX_LOG_LEVEL_DEBUG, module, __VA_ARGS__)

#endif /* SX_COMMON_LOG_H */
