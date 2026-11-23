/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file os.h
 * Monotonic time, memory, synchronization and entropy providers. */
#ifndef RTCP_OS_H
#define RTCP_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Opaque thread handle defined by the platform. */
typedef struct rtcp_thread *rtcp_thread_t;
/** Opaque mutex handle defined by the platform. */
typedef struct rtcp_mutex *rtcp_mutex_t;

/** Borrowed platform operations. Allocated resources are released with
 * the same provider. NULL from an allocator means exhaustion. */
struct rtcp_os
{
    /** Monotonic milliseconds from a stable platform epoch. */
    uint64_t (*now_ms)(void);
    /** Monotonic microseconds from the same epoch. May be
     * NULL on ports with only millisecond resolution, in which case callers
     * fall back to now_ms() * 1000. */
    uint64_t (*now_us)(void);
    /** Block the calling thread for a relative delay in milliseconds. */
    void (*sleep_ms)(uint32_t ms);
    /** Return host-owned allocation storage or NULL. Release with free below. */
    void *(*alloc)(size_t size);
    /** Release storage returned by the matching allocator. */
    void (*free)(void *p);

    /** Start an ordinary platform worker. Return NULL on failure. The
     * caller must keep arg valid until that worker no longer uses it. */
    rtcp_thread_t (*thread_create)(const char *name,
                                   void (*fn)(void *), void *arg,
                                   size_t stack, int prio);
    /** Return an unlocked mutex or NULL on failure. */
    rtcp_mutex_t (*mutex_create)(void);
    /** Wait until the calling thread owns the mutex. */
    void (*mutex_lock)(rtcp_mutex_t m);
    /** Release the mutex from the thread that acquired it. */
    void (*mutex_unlock)(rtcp_mutex_t m);
    /** Destroy an unlocked mutex after all callers have retired. */
    void (*mutex_destroy)(rtcp_mutex_t m);

    /** Cryptographically secure random bytes. NULL when the port has no
     * entropy source; Pair-Setup/Pair-Verify must then refuse to run rather
     * than substitute a weak generator. */
    bool (*random)(uint8_t *output, size_t len);
};

#endif /* RTCP_OS_H */
