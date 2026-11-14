/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "../../port/rtthread/rtcp_os_rtthread.c"
static pthread_mutex_t irq_lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t ticks=UINT32_C(0xffff0000);
rt_base_t rt_hw_interrupt_disable(void) { assert(!pthread_mutex_lock(&irq_lock));return 0; }
void rt_hw_interrupt_enable(rt_base_t state) { (void)state;assert(!pthread_mutex_unlock(&irq_lock)); }
uint32_t rt_tick_get_millisecond(void) { return ticks; }
static void *reader(void *arg)
{
    (void)arg;uint64_t previous=0;
    for(unsigned i=0;i<10000;++i) {
        rt_base_t irq=rt_hw_interrupt_disable();ticks+=1;rt_hw_interrupt_enable(irq);
        uint64_t t=rtcp_os_rtthread.now_us();assert(t>=previous);previous=t;
    }
    return NULL;
}
int main(void)
{
    assert(rtcp_os_rtthread.now_ms()==0);
    ticks=UINT32_MAX-1;assert(rtcp_os_rtthread.now_us()==UINT64_C(65534000));
    ticks=3;assert(rtcp_os_rtthread.now_ms()==65539); /* first wrap at ~65 s */
    ticks=UINT32_MAX;assert(rtcp_os_rtthread.now_ms()==UINT64_C(4295032831));
    ticks=4;assert(rtcp_os_rtthread.now_us()==UINT64_C(4295032836000));
    ticks=UINT32_MAX-100;
    pthread_t a,b;assert(!pthread_create(&a,NULL,reader,NULL));assert(!pthread_create(&b,NULL,reader,NULL));
    assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
    puts("actual RT-Thread clock: startup wrap, repeated wrap, shared ms/us and concurrent readers passed");
    return 0;
}
