/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <stdlib.h>
typedef intptr_t rt_base_t;
typedef int32_t rt_int32_t;
typedef uint32_t rt_uint32_t;
typedef uint8_t rt_uint8_t;
typedef void *rt_thread_t;
typedef void *rt_mutex_t;
#define RT_EOK 0
#define RT_IPC_FLAG_FIFO 0
#define RT_WAITING_FOREVER (-1)
uint32_t rt_tick_get_millisecond(void);
#define rt_malloc malloc
#define rt_free free
static inline void rt_thread_mdelay(rt_int32_t ms) { (void)ms; }
static inline rt_thread_t rt_thread_create(const char *n,void (*fn)(void *),void *a,rt_uint32_t st,rt_uint8_t p,int t)
{ (void)n;(void)fn;(void)a;(void)st;(void)p;(void)t;return NULL; }
static inline int rt_thread_startup(rt_thread_t t) { (void)t;return 0; }
static inline int rt_thread_delete(rt_thread_t t) { (void)t;return 0; }
static inline rt_mutex_t rt_mutex_create(const char *n,int f) { (void)n;(void)f;return NULL; }
static inline int rt_mutex_take(rt_mutex_t m,int w) { (void)m;(void)w;return 0; }
static inline int rt_mutex_release(rt_mutex_t m) { (void)m;return 0; }
static inline int rt_mutex_delete(rt_mutex_t m) { (void)m;return 0; }
