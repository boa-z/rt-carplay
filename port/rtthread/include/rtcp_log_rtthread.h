/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/*
 * RT-Thread logging provider for rt-carplay.
 *
 * The SConscript force-includes this header (-include) so the RTCP_LOGx macros
 * exist before any core header is parsed.  That keeps include/rtcp/log.h free
 * of platform APIs.
 */
#include <rtthread.h>

#define RTCP_LOG_CUSTOM 1

#define RTCP_LOGE(tag, fmt, ...) rt_kprintf("E/%s: " fmt "\n", tag, ##__VA_ARGS__)
#define RTCP_LOGW(tag, fmt, ...) rt_kprintf("W/%s: " fmt "\n", tag, ##__VA_ARGS__)
#define RTCP_LOGI(tag, fmt, ...) rt_kprintf("I/%s: " fmt "\n", tag, ##__VA_ARGS__)
#ifdef RTCP_LOG_DEBUG
#define RTCP_LOGD(tag, fmt, ...) rt_kprintf("D/%s: " fmt "\n", tag, ##__VA_ARGS__)
#else
static inline void rtcp_debug_discard(const char *tag, const char *fmt, ...)
{ (void)tag; (void)fmt; }
#define RTCP_LOGD(tag, ...) do { if (0) rtcp_debug_discard(tag, __VA_ARGS__); } while (0)
#endif
