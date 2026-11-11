/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "../../examples/rtthread_app/audio_sink_lubanlite.c"
typedef struct { pthread_t thread; void (*fn)(void *); void *arg; } thread_t;
static thread_t *worker;
static atomic_uint output_bytes, opened, closed;
static atomic_ullong energy;
static int fail_start;
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
int rt_thread_startup(rt_thread_t a) { if(fail_start)return -1;thread_t *t=a;return pthread_create(&t->thread,NULL,entry,t); }
int rt_thread_delete(rt_thread_t a) { free(a); worker=NULL;return 0; }
rt_device_t rt_device_find(const char *n) { assert(!strcmp(n,"sound0"));return (void *)1; }
int rt_device_open(rt_device_t d,int f) {(void)d;(void)f;++opened;return 0;}
int rt_device_close(rt_device_t d) {(void)d;++closed;return 0;}
int rt_device_control(rt_device_t d,int cmd,void *a) {
 (void)d;if(cmd==AUDIO_CTL_CONFIGURE) {struct rt_audio_caps *c=a;assert(c->udata.config.samplerate==48000 && c->udata.config.channels==2 && c->udata.config.samplebits==16);} return 0;
}
rt_size_t rt_device_write(rt_device_t d,int p,const void *data,rt_size_t n) {
 (void)d;(void)p;assert(n%4==0);const int16_t *pcm=data;
 for(size_t i=0;i<n/2;++i) atomic_fetch_add(&energy,(unsigned long long)((int64_t)pcm[i]*pcm[i]));
 output_bytes+=(unsigned)n;rt_thread_mdelay(5);return n;
}
static void join_worker(void) { if(worker) { assert(!pthread_join(worker->thread,NULL));free(worker);worker=NULL;} }
int main(void) {
 fail_start=1;assert(carplay_audio_sink_start()!=0);assert(!carplay_audio_sink_is_running());
 fail_start=0;
 for(unsigned cycle=0;cycle<3;++cycle) {
  assert(carplay_audio_sink_start()==0);
  rtcp_audio_format_t format={.sample_rate=48000,.channels=1,.clock_rate=48000};
  uint8_t pcm[960];for(size_t j=0;j<sizeof(pcm);j+=2) {pcm[j]=0xe8;pcm[j+1]=3;}
  for(unsigned i=0;i<8;++i) { int rc=carplay_audio_sink_packet(102,&format,pcm,sizeof(pcm),480*i,sink_now_us());assert(rc==0 || rc==RTCP_ETIMEDOUT); }
  rt_thread_mdelay(350);
  carplay_audio_sink_stream_stop(102);
  carplay_audio_sink_stop();join_worker();assert(!carplay_audio_sink_is_running());
 }
 assert(output_bytes>0 && energy>0 && opened==closed);
 puts("actual RT-Thread audio sink: worker playback, failed startup, selective stop and repeated cleanup passed");
}
