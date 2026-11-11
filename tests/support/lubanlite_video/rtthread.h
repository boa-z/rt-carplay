/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef void *rt_mutex_t;
typedef void *rt_sem_t;
typedef void *rt_thread_t;
#define RT_EOK 0
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_FIFO 0
uint32_t rt_tick_get_millisecond(void);
int rt_kprintf(const char *, ...);
int rt_mutex_take(rt_mutex_t, int);
int rt_mutex_release(rt_mutex_t);
rt_mutex_t rt_mutex_create(const char *, int);
int rt_mutex_delete(rt_mutex_t);
int rt_sem_take(rt_sem_t, int);
int rt_sem_release(rt_sem_t);
rt_sem_t rt_sem_create(const char *, unsigned, int);
int rt_sem_delete(rt_sem_t);
void rt_thread_mdelay(int);
rt_thread_t rt_thread_create(const char *, void (*)(void *), void *, unsigned, unsigned, unsigned);
int rt_thread_startup(rt_thread_t);
/* Retain shell entry for warning-clean compilation. */
#define MSH_CMD_EXPORT_ALIAS(fn, alias, desc) \
    static int (*const test_shell_##alias)(int, char **) __attribute__((used)) = fn
