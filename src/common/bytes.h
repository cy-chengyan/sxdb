/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Bounds-checked byte writer and reader.
 *
 * Encoders write through sx_byte_writer and decoders read through sx_byte_reader.
 * Neither ever touches memory outside its buffer: an out-of-range operation sets a
 * sticky failure flag and returns NULL or zero, and the caller checks the flag once at
 * the end. This keeps decoding of untrusted or corrupted data memory-safe without an
 * error check after every field.
 */

#ifndef SX_COMMON_BYTES_H
#define SX_COMMON_BYTES_H

#include "common/bits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct sx_byte_writer {
    uint8_t* data;
    size_t capacity;
    size_t size;
    bool overflow;
} sx_byte_writer;

typedef struct sx_byte_reader {
    const uint8_t* data;
    size_t size;
    size_t pos;
    bool failed;
} sx_byte_reader;

static inline void sx_writer_init(sx_byte_writer* writer, void* data, size_t capacity) {
    writer->data = data;
    writer->capacity = capacity;
    writer->size = 0;
    writer->overflow = false;
}

/* Reserves `count` bytes and returns a pointer to them, or NULL on overflow. */
static inline uint8_t* sx_writer_reserve(sx_byte_writer* writer, size_t count) {
    if (writer->overflow || count > writer->capacity - writer->size) {
        writer->overflow = true;
        return NULL;
    }
    uint8_t* dst = writer->data + writer->size;
    writer->size += count;
    return dst;
}

/* Reserves `count` bytes and fills them with zeros. Returns NULL on overflow. */
static inline uint8_t* sx_writer_reserve_zeroed(sx_byte_writer* writer, size_t count) {
    uint8_t* dst = sx_writer_reserve(writer, count);
    if (dst != NULL) {
        memset(dst, 0, count);
    }
    return dst;
}

static inline void sx_writer_put_bytes(sx_byte_writer* writer, const void* src, size_t count) {
    uint8_t* dst = sx_writer_reserve(writer, count);
    if (dst != NULL && count > 0) {
        memcpy(dst, src, count);
    }
}

static inline void sx_writer_put_u8(sx_byte_writer* writer, uint8_t value) {
    sx_writer_put_bytes(writer, &value, sizeof(value));
}

static inline void sx_writer_put_u16(sx_byte_writer* writer, uint16_t value) {
    sx_writer_put_bytes(writer, &value, sizeof(value));
}

static inline void sx_writer_put_u32(sx_byte_writer* writer, uint32_t value) {
    sx_writer_put_bytes(writer, &value, sizeof(value));
}

static inline void sx_writer_put_u64(sx_byte_writer* writer, uint64_t value) {
    sx_writer_put_bytes(writer, &value, sizeof(value));
}

static inline void sx_writer_put_i64(sx_byte_writer* writer, int64_t value) {
    sx_writer_put_bytes(writer, &value, sizeof(value));
}

static inline void sx_writer_put_f64(sx_byte_writer* writer, double value) {
    sx_writer_put_bytes(writer, &value, sizeof(value));
}

/* Writes zero bytes until the size is a multiple of `alignment`. */
static inline void sx_writer_pad_to(sx_byte_writer* writer, size_t alignment) {
    size_t padded = sx_align_up(writer->size, alignment);
    sx_writer_reserve_zeroed(writer, padded - writer->size);
}

static inline void sx_reader_init(sx_byte_reader* reader, const void* data, size_t size) {
    reader->data = data;
    reader->size = size;
    reader->pos = 0;
    reader->failed = false;
}

/* Consumes `count` bytes and returns a pointer to them, or NULL if not enough remain. */
static inline const uint8_t* sx_reader_take(sx_byte_reader* reader, size_t count) {
    if (reader->failed || count > reader->size - reader->pos) {
        reader->failed = true;
        return NULL;
    }
    const uint8_t* src = reader->data + reader->pos;
    reader->pos += count;
    return src;
}

static inline size_t sx_reader_remaining(const sx_byte_reader* reader) {
    return reader->size - reader->pos;
}

static inline uint8_t sx_reader_get_u8(sx_byte_reader* reader) {
    const uint8_t* src = sx_reader_take(reader, 1);
    if (src == NULL) {
        return 0;
    }
    return src[0];
}

static inline uint16_t sx_reader_get_u16(sx_byte_reader* reader) {
    const uint8_t* src = sx_reader_take(reader, 2);
    if (src == NULL) {
        return 0;
    }
    return sx_load_u16(src);
}

static inline uint32_t sx_reader_get_u32(sx_byte_reader* reader) {
    const uint8_t* src = sx_reader_take(reader, 4);
    if (src == NULL) {
        return 0;
    }
    return sx_load_u32(src);
}

static inline uint64_t sx_reader_get_u64(sx_byte_reader* reader) {
    const uint8_t* src = sx_reader_take(reader, 8);
    if (src == NULL) {
        return 0;
    }
    return sx_load_u64(src);
}

static inline int64_t sx_reader_get_i64(sx_byte_reader* reader) {
    return (int64_t)sx_reader_get_u64(reader);
}

static inline double sx_reader_get_f64(sx_byte_reader* reader) {
    return sx_bits_to_double(sx_reader_get_u64(reader));
}

/* Skips padding so that the position becomes a multiple of `alignment`. */
static inline void sx_reader_skip_to(sx_byte_reader* reader, size_t alignment) {
    size_t padded = sx_align_up(reader->pos, alignment);
    sx_reader_take(reader, padded - reader->pos);
}

#endif /* SX_COMMON_BYTES_H */
