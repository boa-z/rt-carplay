/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * RT-Thread OS seam.  Uses only public RT-Thread APIs so the port works for
 * both the ArtInChip SDK and a plain RT-Thread tree.
 */
#include "rtcp_rtthread.h"

#include <rtthread.h>

#include "rtcp/rtcp.h"

/* Handle types are opaque at the seam; RT-Thread objects are used directly. */

static uint64_t now_ms(void)
{
    return (uint64_t)rt_tick_get_millisecond();
}

static uint64_t now_us(void)
{
    /* RT-Thread ticks are millisecond-scale; expand them rather than inventing
     * sub-tick precision. */
    return (uint64_t)rt_tick_get_millisecond() * 1000U;
}

static void sleep_ms(uint32_t ms)
{
    rt_thread_mdelay((rt_int32_t)ms);
}

static void *os_alloc(size_t size)
{
    return rt_malloc(size);
}

static void os_free(void *p)
{
    rt_free(p);
}

static rtcp_thread_t thread_create(const char *name,
                                   void (*fn)(void *), void *arg,
                                   size_t stack, int prio)
{
    if (fn == NULL) return NULL;
    rt_thread_t thread = rt_thread_create(name,
                                          (void (*)(void *))fn, arg,
                                          (rt_uint32_t)stack,
                                          (rt_uint8_t)prio, 10);
    if (thread == NULL) return NULL;
    if (rt_thread_startup(thread) != RT_EOK) {
        rt_thread_delete(thread);
        return NULL;
    }
    return (rtcp_thread_t)thread;
}

static rtcp_mutex_t mutex_create(void)
{
    return (rtcp_mutex_t)rt_mutex_create("rtcp", RT_IPC_FLAG_FIFO);
}

static void mutex_lock(rtcp_mutex_t mutex)
{
    if (mutex != NULL) (void)rt_mutex_take((rt_mutex_t)mutex, RT_WAITING_FOREVER);
}

static void mutex_unlock(rtcp_mutex_t mutex)
{
    if (mutex != NULL) (void)rt_mutex_release((rt_mutex_t)mutex);
}

static void mutex_destroy(rtcp_mutex_t mutex)
{
    if (mutex != NULL) (void)rt_mutex_delete((rt_mutex_t)mutex);
}

const struct rtcp_os rtcp_os_rtthread = {
    .now_ms = now_ms,
    .now_us = now_us,
    .sleep_ms = sleep_ms,
    .alloc = os_alloc,
    .free = os_free,
    .thread_create = thread_create,
    .mutex_create = mutex_create,
    .mutex_lock = mutex_lock,
    .mutex_unlock = mutex_unlock,
    .mutex_destroy = mutex_destroy,
};
