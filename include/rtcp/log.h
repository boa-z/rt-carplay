/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_LOG_H
#define RTCP_LOG_H

/*
 * Minimal logging seam.  The core is silent unless a platform build defines
 * RTCP_LOG_ENABLE (and optionally RTCP_LOG_TAG).  Platform ports may instead
 * define RTCP_LOG_CUSTOM and provide RTCP_LOGE/W/I/D before including any
 * core header; the RT-Thread port does this through a forced include, so no
 * platform API appears in this directory.
 */
#if defined(RTCP_LOG_CUSTOM)

/* The platform supplies RTCP_LOGE(), RTCP_LOGW(), RTCP_LOGI(), RTCP_LOGD(). */

#elif defined(RTCP_LOG_ENABLE)

#include <stdio.h>

#define RTCP_LOGE(tag, fmt, ...) fprintf(stderr, "E/%s: " fmt "\n", tag, ##__VA_ARGS__)
#define RTCP_LOGW(tag, fmt, ...) fprintf(stderr, "W/%s: " fmt "\n", tag, ##__VA_ARGS__)
#define RTCP_LOGI(tag, fmt, ...) fprintf(stderr, "I/%s: " fmt "\n", tag, ##__VA_ARGS__)
#define RTCP_LOGD(tag, fmt, ...) fprintf(stderr, "D/%s: " fmt "\n", tag, ##__VA_ARGS__)

#else

/*
 * Silent builds still reference their arguments.  A plain "(void)(tag)" drops
 * the format arguments, so variables that exist only for a log line would be
 * reported as "set but not used"; the dead call keeps them used and costs
 * nothing (the body is empty and "if (0)" is folded away).
 */
static inline void rtcp_log_discard(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}

#define RTCP_LOGE(tag, ...) do { if (0) rtcp_log_discard(tag, __VA_ARGS__); } while (0)
#define RTCP_LOGW(tag, ...) do { if (0) rtcp_log_discard(tag, __VA_ARGS__); } while (0)
#define RTCP_LOGI(tag, ...) do { if (0) rtcp_log_discard(tag, __VA_ARGS__); } while (0)
#define RTCP_LOGD(tag, ...) do { if (0) rtcp_log_discard(tag, __VA_ARGS__); } while (0)

#endif

#endif /* RTCP_LOG_H */
