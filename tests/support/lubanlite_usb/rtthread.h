/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
void *rt_malloc_align(size_t size, size_t align);
void rt_free_align(void *p);
int rt_kprintf(const char *fmt, ...);
