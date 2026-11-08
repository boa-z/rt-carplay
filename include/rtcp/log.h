/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_LOG_H
#define RTCP_LOG_H

/*
 * Minimal logging seam.  The core is silent unless a platform build defines
 * RTCP_LOG_ENABLE (and optionally RTCP_LOG_TAG).  Platform ports may instead
 * provide macros of their own before including core headers.
 */
#ifdef RTCP_LOG_ENABLE

#include <stdio.h>

#define RTCP_LOGE(tag, fmt, ...) fprintf(stderr, "E/%s: " fmt "\n", tag, ##__VA_ARGS__)
#define RTCP_LOGW(tag, fmt, ...) fprintf(stderr, "W/%s: " fmt "\n", tag, ##__VA_ARGS__)
#define RTCP_LOGI(tag, fmt, ...) fprintf(stderr, "I/%s: " fmt "\n", tag, ##__VA_ARGS__)
#define RTCP_LOGD(tag, fmt, ...) fprintf(stderr, "D/%s: " fmt "\n", tag, ##__VA_ARGS__)

#else

#define RTCP_LOGE(tag, ...) ((void)(tag))
#define RTCP_LOGW(tag, ...) ((void)(tag))
#define RTCP_LOGI(tag, ...) ((void)(tag))
#define RTCP_LOGD(tag, ...) ((void)(tag))

#endif

#endif /* RTCP_LOG_H */
