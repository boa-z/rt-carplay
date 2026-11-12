/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "rtthread.h"
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
typedef struct { pthread_t thread; void (*fn)(void *); void *arg; } thread_t;
static thread_t *worker;
int host_rtthread_fail_start;
int rt_kprintf(const char *f,...) { (void)f; return 0; }
rt_tick_t rt_tick_get(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (rt_tick_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000); }
rt_tick_t rt_tick_from_millisecond(int n) { return (rt_tick_t)n; }
void rt_thread_mdelay(int ms) { struct timespec t={ms/1000,(ms%1000)*1000000L}; nanosleep(&t,NULL); }
rt_mutex_t rt_mutex_create(const char *n,int f) { (void)n;(void)f; pthread_mutex_t *m=malloc(sizeof(*m)); assert(!pthread_mutex_init(m,NULL)); return m; }
int rt_mutex_take(rt_mutex_t m,int wait) { return wait==0 ? pthread_mutex_trylock(m) : pthread_mutex_lock(m); }
int rt_mutex_release(rt_mutex_t m) { return pthread_mutex_unlock(m); }
int rt_mutex_delete(rt_mutex_t m) { assert(!pthread_mutex_destroy(m));free(m);return 0; }
rt_sem_t rt_sem_create(const char *n,unsigned v,int f) { (void)n;(void)f;sem_t *s=malloc(sizeof(*s));assert(!sem_init(s,0,v));return s; }
int rt_sem_take(rt_sem_t s,int wait) {
 struct timespec t; clock_gettime(CLOCK_REALTIME,&t);t.tv_nsec+=(long)wait*1000000;
 t.tv_sec+=t.tv_nsec/1000000000;t.tv_nsec%=1000000000;
 int rc;do{rc=sem_timedwait(s,&t);}while(rc && errno==EINTR);return rc;
}
int rt_sem_release(rt_sem_t s) { return sem_post(s); }
int rt_sem_delete(rt_sem_t s) { assert(!sem_destroy(s)); free(s);return 0; }
rt_thread_t rt_thread_create(const char *n,void (*fn)(void *),void *a,unsigned st,unsigned p,unsigned tick) {
 (void)n;(void)st;(void)p;(void)tick; assert(!worker);worker=calloc(1,sizeof(*worker));worker->fn=fn;worker->arg=a;return worker;
}
static void *entry(void *a){thread_t *t=a;t->fn(t->arg);return NULL;}
int rt_thread_startup(rt_thread_t a) { if(host_rtthread_fail_start)return -1;thread_t *t=a;return pthread_create(&t->thread,NULL,entry,t); }
int rt_thread_delete(rt_thread_t a) { free(a); worker=NULL;return 0; }
void host_rtthread_join(void) { if(worker) { assert(!pthread_join(worker->thread,NULL));free(worker);worker=NULL;} }

/* Virtual source and receiver arrivals share the port's monotonic epoch. */
#include "rtcp/os.h"
static uint64_t host_now_us(void) { return (uint64_t)rt_tick_get()*1000; }
const struct rtcp_os rtcp_os_rtthread={.now_us=host_now_us};
