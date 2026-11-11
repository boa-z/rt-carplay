/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <sys/ioctl.h>
typedef void *rt_device_t;
rt_device_t rt_device_find(const char *);
int rt_device_control(rt_device_t, int, void *);
