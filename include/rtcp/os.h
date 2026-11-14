/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_OS_H
#define RTCP_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rtcp_thread *rtcp_thread_t;
typedef struct rtcp_mutex  *rtcp_mutex_t;

struct rtcp_os {
    uint64_t (*now_ms)(void);
    /* Monotonic microseconds; used by the AirPlay timing exchange.  May be
     * NULL on ports with only millisecond resolution, in which case callers
     * fall back to now_ms() * 1000. */
    uint64_t (*now_us)(void);
    void     (*sleep_ms)(uint32_t ms);
    void    *(*alloc)(size_t size);
    void     (*free)(void *p);

    rtcp_thread_t (*thread_create)(const char *name,
                                   void (*fn)(void *), void *arg,
                                   size_t stack, int prio);
    rtcp_mutex_t  (*mutex_create)(void);
    void          (*mutex_lock)(rtcp_mutex_t m);
    void          (*mutex_unlock)(rtcp_mutex_t m);
    void          (*mutex_destroy)(rtcp_mutex_t m);

    /* Cryptographically secure random bytes.  NULL when the port has no
     * entropy source; Pair-Setup/Pair-Verify must then refuse to run rather
     * than substitute a weak generator. */
    bool          (*random)(uint8_t *output, size_t len);
};

#endif /* RTCP_OS_H */
