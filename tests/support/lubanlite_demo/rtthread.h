/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
uint32_t rt_tick_get_millisecond(void);
typedef void *rt_mutex_t;
typedef void *rt_thread_t;
#define RT_EOK 0
#define RT_WAITING_FOREVER (-1)
#define RT_WAITING_NO 0
#define RT_IPC_FLAG_PRIO 1
int rt_mutex_take(rt_mutex_t, int);
int rt_mutex_release(rt_mutex_t);
rt_mutex_t rt_mutex_create(const char *, int);
int rt_kprintf(const char *, ...);
void rt_thread_mdelay(int);
rt_thread_t rt_thread_create(const char *, void (*)(void *), void *, unsigned, unsigned, unsigned);
int rt_thread_startup(rt_thread_t);
int rt_thread_delete(rt_thread_t);
