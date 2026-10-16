/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * CRC32C (Castagnoli polynomial), used to checksum pages, WAL records, and MANIFEST
 * records. Uses the SSE4.2 or ARMv8 CRC instructions when the CPU has them and a
 * table-driven software implementation otherwise.
 */

#ifndef SX_COMMON_CRC32C_H
#define SX_COMMON_CRC32C_H

#include <stddef.h>
#include <stdint.h>

/*
 * Extends `crc` with `size` bytes of `data`. Start with crc = 0; the result of one call
 * can be passed to the next to checksum data in pieces.
 */
uint32_t sx_crc32c(uint32_t crc, const void* data, size_t size);

/* The portable implementation, exposed so tests can compare it with the hardware one. */
uint32_t sx_crc32c_software(uint32_t crc, const void* data, size_t size);

#endif /* SX_COMMON_CRC32C_H */
