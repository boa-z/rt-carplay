/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file rtcp.h
 * Shared result codes. Zero denotes success and negative values are errors. */
#ifndef RTCP_H
#define RTCP_H

#include <stddef.h>
#include <stdint.h>

#include "rtcp/version.h"

#define RTCP_OK        0  /**< Success. */
#define RTCP_EIO       -1 /**< Transport or provider failure. */
#define RTCP_EINVAL    -2 /**< Invalid argument or malformed input. */
#define RTCP_ENOTSUP   -3 /**< Unsupported feature or operation. */
#define RTCP_ETIMEDOUT -4 /**< Deadline expired before completion. */
#define RTCP_ENOMEM    -5 /**< Allocation, buffer or queue capacity exhausted. */
#define RTCP_ENOENT    -6 /**< Requested object or pending work is absent. */
#define RTCP_ESTATE    -7 /**< Operation is invalid in the current state. */

#endif /* RTCP_H */
