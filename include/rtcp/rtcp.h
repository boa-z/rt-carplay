/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_H
#define RTCP_H

#include <stddef.h>
#include <stdint.h>

#define RTCP_OK          0
#define RTCP_EIO        -1
#define RTCP_EINVAL     -2
#define RTCP_ENOTSUP    -3
#define RTCP_ETIMEDOUT  -4
#define RTCP_ENOMEM     -5

#define RTCP_VERSION_MAJOR 0
#define RTCP_VERSION_MINOR 1

#endif /* RTCP_H */
