/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Internal error codes. Functions that can fail return sx_err; SX_OK is zero so that
 * "if (err != SX_OK)" and "if (err)" read the same.
 */

#ifndef SX_COMMON_ERROR_H
#define SX_COMMON_ERROR_H

typedef enum sx_err {
    SX_OK = 0,
    SX_ERR_NOMEM,       /* allocation failed or the memory limit was reached */
    SX_ERR_INVALID_ARG, /* an argument or configuration value is invalid */
    SX_ERR_CORRUPT,     /* encoded or on-disk data is malformed */
    SX_ERR_IO,          /* an operating system I/O call failed */
    SX_ERR_LIMIT,       /* a configured limit would be exceeded */
    SX_ERR_UNSUPPORTED, /* the operation is not supported */
    SX_ERR_NOT_FOUND,   /* a file or directory does not exist */
    SX_ERR_EXISTS,      /* a file or directory already exists */
    SX_ERR_PERMISSION,  /* the operating system denied access */
    SX_ERR_NO_SPACE,    /* the file system is full or a disk quota was reached */
    SX_ERR_BUSY,        /* another process holds the resource, such as a data directory lock */
} sx_err;

/* Returns a short English description of the error code. */
const char* sx_err_str(sx_err err);

#endif /* SX_COMMON_ERROR_H */
