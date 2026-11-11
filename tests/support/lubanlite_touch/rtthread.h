/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../lubanlite_video/rtthread.h"
typedef int rt_err_t;
typedef size_t rt_size_t;
#define RT_NULL NULL
int rt_tick_from_millisecond(unsigned);
int rt_thread_delete(rt_thread_t);
void *rt_malloc(size_t);
void rt_free(void *);
