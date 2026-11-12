/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <rtthread.h>
/* RV64 FAAD stereo LC has >20 KiB of nested transform scratch alone.
 * Keep decoder + RTOS/libc call headroom; callers keep large I/O in workspace. */
#define CARPLAY_AUDIO_WORKER_STACK_BYTES (96U * 1024U)
static inline void carplay_audio_stack_report(const char *owner)
{
#if defined(RT_VERSION) && !defined(ARCH_CPU_STACK_GROWS_UPWARD)
    rt_thread_t self=rt_thread_self();
    if(!self)return;
    const unsigned char *base=(const unsigned char *)self->stack_addr;
    unsigned unused=0,total=(unsigned)self->stack_size;
    /* RT-Thread initializes descending stacks with '#'. This is a watermark,
     * not the current SP or proof that every possible codec input is bounded. */
    while(unused<total && base[unused]=='#')++unused;
    rt_kprintf("[%s] stack total=%u used_peak=%u free_min=%u bytes\n",
               owner,total,total-unused,unused);
#else
    (void)owner;
#endif
}
