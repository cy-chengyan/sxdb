/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Parsing of configuration values.
 */

#ifndef SX_COMMON_PARSE_H
#define SX_COMMON_PARSE_H

#include "common/error.h"

#include <stdint.h>

/*
 * Parses a byte size such as "1024", "64K", "512M", "8G", or "1T". Units are powers of
 * 1024 and may be upper or lower case. Nothing else is accepted: no spaces, no "B"
 * suffix, no fractions, no percentages. Returns SX_ERR_INVALID_ARG on malformed input
 * or overflow.
 */
sx_err sx_parse_size(const char* text, uint64_t* bytes);

#endif /* SX_COMMON_PARSE_H */
