/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "common/log.h"

#include "common/clock.h"
#include "os/os.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Longest line kept, including the trailing newline. Longer messages are truncated. */
#define LOG_LINE_MAX    1024u
#define LOG_QUEUE_SLOTS 256u
#define LOG_PATH_MAX    1024u
#define LOG_NAME_MAX    64u

/* "20261016T080000Z" */
#define LOG_STAMP_LENGTH 16u

typedef struct log_line {
    uint32_t size;
    char text[LOG_LINE_MAX];
} log_line;

typedef struct log_files {
    char dir[LOG_PATH_MAX];
    char name[LOG_NAME_MAX];
    uint64_t file_size;
    uint64_t total_size;
    int fd;
    uint64_t current_size;
} log_files;

typedef struct rotated_file {
    char entry_name[LOG_NAME_MAX + 48];
    char stamp[LOG_STAMP_LENGTH + 1];
    unsigned long suffix;
    uint64_t size;
} rotated_file;

/*
 * Logger state. `running` and the queue are protected by `mutex`; the file state is
 * touched only by the writer thread (and by open/close while no writer runs).
 */
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_t writer;
    bool running;
    bool stopping;
    sx_log_output output;
    log_line* queue;
    uint32_t head;
    uint32_t count;
    log_files files;
} logger = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
    .not_full = PTHREAD_COND_INITIALIZER,
    .files = {.fd = -1},
};

static _Atomic int current_level = SX_LOG_LEVEL_WARN;
static _Atomic uint64_t dropped_lines = 0;

/* ---------------------------------------------------------------------------------------- */
/* Levels                                                                                   */
/* ---------------------------------------------------------------------------------------- */

const char* sx_log_level_name(sx_log_level level) {
    switch (level) {
        case SX_LOG_LEVEL_ERROR:
            return "ERROR";
        case SX_LOG_LEVEL_WARN:
            return "WARN";
        case SX_LOG_LEVEL_INFO:
            return "INFO";
        case SX_LOG_LEVEL_DEBUG:
            return "DEBUG";
    }
    return "UNKNOWN";
}

sx_err sx_log_parse_level(const char* text, sx_log_level* level) {
    static const struct {
        const char* text;
        sx_log_level level;
    } levels[] = {
        {"error", SX_LOG_LEVEL_ERROR},
        {"warn", SX_LOG_LEVEL_WARN},
        {"info", SX_LOG_LEVEL_INFO},
        {"debug", SX_LOG_LEVEL_DEBUG},
    };
    for (size_t i = 0; i < SX_ARRAY_SIZE(levels); i++) {
        if (strcmp(text, levels[i].text) == 0) {
            *level = levels[i].level;
            return SX_OK;
        }
    }
    return SX_ERR_INVALID_ARG;
}

void sx_log_set_level(sx_log_level level) {
    atomic_store(&current_level, (int)level);
}

bool sx_log_enabled(sx_log_level level) {
    return (int)level <= atomic_load_explicit(&current_level, memory_order_relaxed);
}

uint64_t sx_log_dropped_count(void) {
    return atomic_load(&dropped_lines);
}

/* ---------------------------------------------------------------------------------------- */
/* Line formatting                                                                          */
/* ---------------------------------------------------------------------------------------- */

/*
 * Copies `message` into `dst`, escaping newlines and other control characters so that
 * one event always stays on one line. Returns the number of bytes written. If the
 * message does not fit, it is cut and ends with "...".
 */
static size_t escape_message(char* dst, size_t capacity, const char* message) {
    static const char hex_digits[] = "0123456789abcdef";
    const size_t reserve_for_ellipsis = 3;
    size_t used = 0;

    for (const unsigned char* p = (const unsigned char*)message; *p != '\0'; p++) {
        char escaped[4];
        size_t escaped_size = 0;
        if (*p == '\n') {
            escaped[0] = '\\';
            escaped[1] = 'n';
            escaped_size = 2;
        } else if (*p == '\r') {
            escaped[0] = '\\';
            escaped[1] = 'r';
            escaped_size = 2;
        } else if (*p == '\t') {
            escaped[0] = '\\';
            escaped[1] = 't';
            escaped_size = 2;
        } else if (*p < 0x20u || *p == 0x7Fu) {
            escaped[0] = '\\';
            escaped[1] = 'x';
            escaped[2] = hex_digits[*p >> 4];
            escaped[3] = hex_digits[*p & 0x0Fu];
            escaped_size = 4;
        } else {
            escaped[0] = (char)*p;
            escaped_size = 1;
        }

        if (used + escaped_size + reserve_for_ellipsis > capacity) {
            memcpy(dst + used, "...", reserve_for_ellipsis);
            return used + reserve_for_ellipsis;
        }
        memcpy(dst + used, escaped, escaped_size);
        used += escaped_size;
    }
    return used;
}

SX_PRINTF_FORMAT(4, 0)
static void format_line(log_line* line, sx_log_level level, const char* module, const char* format,
                        va_list args) {
    char timestamp[SX_TIMESTAMP_TEXT_SIZE];
    sx_format_utc_millis(sx_clock_realtime_ns(), timestamp);

    int prefix = snprintf(line->text, LOG_LINE_MAX, "%s %-5s [%s] ", timestamp,
                          sx_log_level_name(level), module);
    if (prefix < 0 || (size_t)prefix >= LOG_LINE_MAX / 2) {
        prefix = 0;
    }

    char message[LOG_LINE_MAX];
    vsnprintf(message, sizeof(message), format, args);

    /* Leave one byte for the newline. */
    size_t capacity = LOG_LINE_MAX - (size_t)prefix - 1u;
    size_t used = (size_t)prefix + escape_message(line->text + prefix, capacity, message);
    line->text[used] = '\n';
    line->size = (uint32_t)(used + 1u);
}

/* ---------------------------------------------------------------------------------------- */
/* Log files: rotation and retention                                                        */
/* ---------------------------------------------------------------------------------------- */

static bool is_digits(const char* text, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

/*
 * Recognizes "<name>-YYYYMMDDTHHMMSSZ.log" and "<name>-YYYYMMDDTHHMMSSZ-<n>.log" and
 * extracts the time stamp and the optional suffix number. Only files matching this
 * pattern are ever deleted.
 */
static bool parse_rotated_name(const char* entry, const char* name, rotated_file* out) {
    size_t name_length = strlen(name);
    if (strncmp(entry, name, name_length) != 0 || entry[name_length] != '-') {
        return false;
    }
    const char* stamp = entry + name_length + 1;
    if (strlen(stamp) < LOG_STAMP_LENGTH || !is_digits(stamp, 8) || stamp[8] != 'T' ||
        !is_digits(stamp + 9, 6) || stamp[15] != 'Z') {
        return false;
    }

    const char* rest = stamp + LOG_STAMP_LENGTH;
    unsigned long suffix = 0;
    if (rest[0] == '-') {
        char* end = NULL;
        if (!is_digits(rest + 1, 1)) {
            return false;
        }
        suffix = strtoul(rest + 1, &end, 10);
        rest = end;
    }
    if (strcmp(rest, ".log") != 0) {
        return false;
    }

    memcpy(out->stamp, stamp, LOG_STAMP_LENGTH);
    out->stamp[LOG_STAMP_LENGTH] = '\0';
    out->suffix = suffix;
    return true;
}

/* Oldest first: by time stamp, then by suffix (a suffix marks a later rotation). */
static int compare_rotated(const void* left, const void* right) {
    const rotated_file* a = left;
    const rotated_file* b = right;
    int by_stamp = strcmp(a->stamp, b->stamp);
    if (by_stamp != 0) {
        return by_stamp;
    }
    if (a->suffix < b->suffix) {
        return -1;
    }
    if (a->suffix > b->suffix) {
        return 1;
    }
    return 0;
}

static void join_path(char* out, size_t capacity, const char* dir, const char* entry) {
    snprintf(out, capacity, "%s/%s", dir, entry);
}

/*
 * Deletes the oldest rotated files until the rotated files plus one full current file
 * fit within total_size, so the total never exceeds the limit as the current file grows.
 */
static void enforce_retention(log_files* files) {
    DIR* dir = opendir(files->dir);
    if (dir == NULL) {
        return;
    }

    rotated_file* rotated = NULL;
    size_t count = 0;
    size_t capacity = 0;
    uint64_t rotated_total = 0;

    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        rotated_file candidate;
        size_t entry_length = strlen(entry->d_name);
        if (entry_length >= sizeof(candidate.entry_name) ||
            !parse_rotated_name(entry->d_name, files->name, &candidate)) {
            continue;
        }
        char path[LOG_PATH_MAX + LOG_NAME_MAX + 48];
        struct stat info;
        join_path(path, sizeof(path), files->dir, entry->d_name);
        if (stat(path, &info) != 0) {
            continue;
        }
        if (count == capacity) {
            size_t new_capacity = capacity == 0 ? 16 : capacity * 2;
            rotated_file* grown = realloc(rotated, new_capacity * sizeof(*grown));
            if (grown == NULL) {
                break;
            }
            rotated = grown;
            capacity = new_capacity;
        }
        /*
         * The length was checked above. Copying it directly, rather than with snprintf from
         * the larger d_name array, keeps GCC from reporting a truncation that cannot happen.
         */
        memcpy(candidate.entry_name, entry->d_name, entry_length + 1);
        candidate.size = (uint64_t)info.st_size;
        rotated[count] = candidate;
        count++;
        rotated_total += candidate.size;
    }
    closedir(dir);

    if (count > 0) {
        qsort(rotated, count, sizeof(*rotated), compare_rotated);
    }

    uint64_t budget = files->total_size - files->file_size;
    for (size_t i = 0; i < count && rotated_total > budget; i++) {
        char path[LOG_PATH_MAX + LOG_NAME_MAX + 48];
        join_path(path, sizeof(path), files->dir, rotated[i].entry_name);
        if (unlink(path) == 0) {
            rotated_total -= rotated[i].size;
        }
    }
    free(rotated);
}

static sx_err open_current_file(log_files* files) {
    char path[LOG_PATH_MAX + LOG_NAME_MAX + 8];
    snprintf(path, sizeof(path), "%s/%s.log", files->dir, files->name);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        return SX_ERR_IO;
    }
    struct stat info;
    if (fstat(fd, &info) != 0) {
        close(fd);
        return SX_ERR_IO;
    }
    files->fd = fd;
    files->current_size = (uint64_t)info.st_size;
    return SX_OK;
}

/* Renames the current file to "<name>-<UTC time>[-n].log" and starts a new one. */
static void rotate(log_files* files) {
    close(files->fd);
    files->fd = -1;

    char stamp[SX_TIMESTAMP_TEXT_SIZE];
    sx_format_utc_compact(sx_clock_realtime_ns(), stamp);

    char current_path[LOG_PATH_MAX + LOG_NAME_MAX + 8];
    char rotated_path[LOG_PATH_MAX + LOG_NAME_MAX + 48];
    snprintf(current_path, sizeof(current_path), "%s/%s.log", files->dir, files->name);
    snprintf(rotated_path, sizeof(rotated_path), "%s/%s-%s.log", files->dir, files->name, stamp);

    /* Several rotations within one second get increasing suffixes. */
    struct stat info;
    for (unsigned suffix = 1; stat(rotated_path, &info) == 0 && suffix < 100000; suffix++) {
        snprintf(rotated_path, sizeof(rotated_path), "%s/%s-%s-%u.log", files->dir, files->name,
                 stamp, suffix);
    }
    rename(current_path, rotated_path);

    if (open_current_file(files) != SX_OK) {
        files->fd = -1;
    }
    enforce_retention(files);
}

static void write_to_file(log_files* files, const log_line* line) {
    if (files->current_size > 0 && files->current_size + line->size > files->file_size) {
        rotate(files);
    }
    if (files->fd < 0) {
        return;
    }
    if (sx_os_write_all(files->fd, line->text, line->size) == SX_OK) {
        files->current_size += line->size;
    }
}

/* ---------------------------------------------------------------------------------------- */
/* Queue and writer thread                                                                  */
/* ---------------------------------------------------------------------------------------- */

static void write_line(const log_line* line) {
    if (logger.output == SX_LOG_OUTPUT_FILE) {
        write_to_file(&logger.files, line);
    } else {
        sx_os_write_all(STDERR_FILENO, line->text, line->size);
    }
}

static void* writer_main(void* arg) {
    SX_UNUSED(arg);
    log_line line;

    pthread_mutex_lock(&logger.mutex);
    for (;;) {
        while (logger.count == 0 && !logger.stopping) {
            pthread_cond_wait(&logger.not_empty, &logger.mutex);
        }
        if (logger.count == 0) {
            break;
        }
        line = logger.queue[logger.head];
        logger.head = (logger.head + 1u) % LOG_QUEUE_SLOTS;
        logger.count--;
        pthread_cond_signal(&logger.not_full);

        pthread_mutex_unlock(&logger.mutex);
        write_line(&line);
        pthread_mutex_lock(&logger.mutex);
    }
    pthread_mutex_unlock(&logger.mutex);
    return NULL;
}

/* Called with the mutex held. Drops INFO and DEBUG lines when the queue is full. */
static void enqueue_locked(const log_line* line, sx_log_level level) {
    while (logger.count == LOG_QUEUE_SLOTS) {
        if (level >= SX_LOG_LEVEL_INFO) {
            atomic_fetch_add(&dropped_lines, 1);
            return;
        }
        pthread_cond_wait(&logger.not_full, &logger.mutex);
    }
    uint32_t tail = (logger.head + logger.count) % LOG_QUEUE_SLOTS;
    logger.queue[tail] = *line;
    logger.count++;
    pthread_cond_signal(&logger.not_empty);
}

void sx_log_write(sx_log_level level, const char* module, const char* format, ...) {
    if (!sx_log_enabled(level)) {
        return;
    }
    log_line line;
    va_list args;
    va_start(args, format);
    format_line(&line, level, module, format, args);
    va_end(args);

    pthread_mutex_lock(&logger.mutex);
    if (logger.running) {
        enqueue_locked(&line, level);
    } else {
        sx_os_write_all(STDERR_FILENO, line.text, line.size);
    }
    pthread_mutex_unlock(&logger.mutex);
}

/* ---------------------------------------------------------------------------------------- */
/* Open and close                                                                           */
/* ---------------------------------------------------------------------------------------- */

static bool is_valid_name(const char* name) {
    size_t length = strlen(name);
    if (length == 0 || length >= LOG_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        bool allowed =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

static sx_err validate_config(const sx_log_config* config) {
    if (config->level < SX_LOG_LEVEL_ERROR || config->level > SX_LOG_LEVEL_DEBUG) {
        return SX_ERR_INVALID_ARG;
    }
    if (config->output == SX_LOG_OUTPUT_STDERR) {
        return SX_OK;
    }
    if (config->output != SX_LOG_OUTPUT_FILE || config->dir == NULL || config->name == NULL) {
        return SX_ERR_INVALID_ARG;
    }
    if (strlen(config->dir) >= LOG_PATH_MAX || !is_valid_name(config->name)) {
        return SX_ERR_INVALID_ARG;
    }
    if (config->file_size == 0 || config->total_size < config->file_size) {
        return SX_ERR_INVALID_ARG;
    }
    return SX_OK;
}

sx_err sx_log_open(const sx_log_config* config) {
    sx_err err = validate_config(config);
    if (err != SX_OK) {
        return err;
    }
    sx_log_close();

    log_line* queue = calloc(LOG_QUEUE_SLOTS, sizeof(*queue));
    if (queue == NULL) {
        return SX_ERR_NOMEM;
    }

    log_files files = {.fd = -1};
    if (config->output == SX_LOG_OUTPUT_FILE) {
        snprintf(files.dir, sizeof(files.dir), "%s", config->dir);
        snprintf(files.name, sizeof(files.name), "%s", config->name);
        files.file_size = config->file_size;
        files.total_size = config->total_size;
        err = open_current_file(&files);
        if (err != SX_OK) {
            free(queue);
            return err;
        }
        enforce_retention(&files);
    }

    pthread_mutex_lock(&logger.mutex);
    logger.queue = queue;
    logger.head = 0;
    logger.count = 0;
    logger.output = config->output;
    logger.files = files;
    logger.stopping = false;
    if (pthread_create(&logger.writer, NULL, writer_main, NULL) != 0) {
        logger.queue = NULL;
        logger.files.fd = -1;
        pthread_mutex_unlock(&logger.mutex);
        if (files.fd >= 0) {
            close(files.fd);
        }
        free(queue);
        return SX_ERR_NOMEM;
    }
    logger.running = true;
    sx_log_set_level(config->level);
    pthread_mutex_unlock(&logger.mutex);
    return SX_OK;
}

void sx_log_close(void) {
    pthread_mutex_lock(&logger.mutex);
    if (!logger.running) {
        pthread_mutex_unlock(&logger.mutex);
        return;
    }
    logger.stopping = true;
    pthread_cond_signal(&logger.not_empty);
    pthread_mutex_unlock(&logger.mutex);

    pthread_join(logger.writer, NULL);

    pthread_mutex_lock(&logger.mutex);
    /* Lines queued after the writer exited are written here, so nothing is lost. */
    while (logger.count > 0) {
        write_line(&logger.queue[logger.head]);
        logger.head = (logger.head + 1u) % LOG_QUEUE_SLOTS;
        logger.count--;
    }
    if (logger.files.fd >= 0) {
        close(logger.files.fd);
        logger.files.fd = -1;
    }
    free(logger.queue);
    logger.queue = NULL;
    logger.running = false;
    logger.stopping = false;
    /* Wake any producer that was waiting for space; it will now write to stderr. */
    pthread_cond_broadcast(&logger.not_full);
    pthread_mutex_unlock(&logger.mutex);
}
