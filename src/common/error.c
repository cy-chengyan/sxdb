/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "common/error.h"

const char* sx_err_str(sx_err err) {
    switch (err) {
        case SX_OK:
            return "success";
        case SX_ERR_NOMEM:
            return "out of memory";
        case SX_ERR_INVALID_ARG:
            return "invalid argument";
        case SX_ERR_CORRUPT:
            return "data is corrupted";
        case SX_ERR_IO:
            return "input/output error";
        case SX_ERR_LIMIT:
            return "limit exceeded";
        case SX_ERR_UNSUPPORTED:
            return "operation not supported";
    }
    return "unknown error";
}
