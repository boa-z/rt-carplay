/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "rtthread.h"
rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t);
