/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_OS_H
#define RTCP_OS_H

#include <stddef.h>
#include <stdint.h>

typedef struct rtcp_thread *rtcp_thread_t;
typedef struct rtcp_mutex  *rtcp_mutex_t;

struct rtcp_os {
    uint64_t (*now_ms)(void);
    void     (*sleep_ms)(uint32_t ms);
    void    *(*alloc)(size_t size);
    void     (*free)(void *p);

    rtcp_thread_t (*thread_create)(const char *name,
                                   void (*fn)(void *), void *arg,
                                   size_t stack, int prio);
    rtcp_mutex_t  (*mutex_create)(void);
    void          (*mutex_lock)(rtcp_mutex_t m);
    void          (*mutex_unlock)(rtcp_mutex_t m);
};

#endif /* RTCP_OS_H */
