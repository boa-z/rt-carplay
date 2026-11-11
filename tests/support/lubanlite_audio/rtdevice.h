/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "rtthread.h"
typedef void *rt_device_t;
#define RT_DEVICE_OFLAG_WRONLY 2
rt_device_t rt_device_find(const char *);
int rt_device_open(rt_device_t,int);
int rt_device_close(rt_device_t);
int rt_device_control(rt_device_t,int,void *);
rt_size_t rt_device_write(rt_device_t,int,const void *,rt_size_t);
