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
extern int host_rtthread_fail_start;
void host_rtthread_join(void);
static atomic_uint output_bytes, opened, closed;
static atomic_ullong energy;
static atomic_int expected_sample;
static atomic_bool check_samples;
rt_device_t rt_device_find(const char *n) { assert(!strcmp(n,"sound0"));return (void *)1; }
int rt_device_open(rt_device_t d,int f) {(void)d;(void)f;++opened;return 0;}
int rt_device_close(rt_device_t d) {(void)d;++closed;return 0;}
int rt_device_control(rt_device_t d,int cmd,void *a) {
 (void)d;if(cmd==AUDIO_CTL_CONFIGURE) {struct rt_audio_caps *c=a;assert(c->udata.config.samplerate==48000 && c->udata.config.channels==2 && c->udata.config.samplebits==16);} return 0;
}
rt_size_t rt_device_write(rt_device_t d,int p,const void *data,rt_size_t n) {
 (void)d;(void)p;assert(n%4==0);const int16_t *pcm=data;
 for(size_t i=0;i<n/2;++i) { if(check_samples && pcm[i]!=expected_sample) {fprintf(stderr,"sample=%d expected=%d offset=%zu\n",pcm[i],(int)expected_sample,i);assert(0); } }
 for(size_t i=0;i<n/2;++i) atomic_fetch_add(&energy,(unsigned long long)((int64_t)pcm[i]*pcm[i]));
 output_bytes+=(unsigned)n;rt_thread_mdelay(5);return n;
}

int main(void) {
 host_rtthread_fail_start=1;assert(carplay_audio_sink_start()!=0);assert(!carplay_audio_sink_is_running());
 host_rtthread_fail_start=0;
 for(unsigned cycle=0;cycle<3;++cycle) {
  assert(carplay_audio_sink_start()==0);
  rtcp_audio_format_t format={.sample_rate=48000,.channels=1,.clock_rate=48000};
  uint8_t pcm[960];for(size_t j=0;j<sizeof(pcm);j+=2) {pcm[j]=0xe8;pcm[j+1]=3;}
  for(unsigned i=0;i<8;++i) { int rc=carplay_audio_sink_packet(102,&format,pcm,sizeof(pcm),480*i,sink_now_us());assert(rc==0 || rc==RTCP_ETIMEDOUT); }
  if(!cycle) {
   int rc=RTCP_ETIMEDOUT;
   for(unsigned retry=0;retry<20 && rc==RTCP_ETIMEDOUT;++retry) {
    rc=carplay_audio_sink_local_pcm(pcm,sizeof(pcm),48000,1,true);
    if(rc==RTCP_ETIMEDOUT)rt_thread_mdelay(1);
   }
   assert(rc==RTCP_OK);
   assert(carplay_audio_sink_local_pcm(pcm,sizeof(pcm),48000,1,false)==RTCP_ESTATE);
  }
  rt_thread_mdelay(350);
  carplay_audio_sink_stream_stop(102);
  carplay_audio_sink_stop();host_rtthread_join();assert(!carplay_audio_sink_is_running());
 }
 /* Actual worker: 1024-frame 44.1 kHz bursts and local volume command.
  * A nonblocking test device exposes artificial silence at packet tails. */
 for(unsigned level=0;level<3;++level) {
  assert(carplay_audio_sink_start()==0);
  const char *value=level==0?"100":level==1?"50":"0";
  char *args[]={"carplay_volume",(char *)value}; volume_command(2,args);
  assert(carplay_audio_sink_volume_percent()==(level==0?100:level==1?50:0));
  /* Same Q15 gain used by the sink, including quantization. */
  unsigned gain=level==0?32768:level==1?(unsigned)(powf(10,-22.5f/20)*32768):0;
  expected_sample=(int)(1000*gain/32768);check_samples=true;
  unsigned before=output_bytes;
  rtcp_audio_format_t f={.sample_rate=44100,.channels=2,.clock_rate=44100};
  uint8_t pcm[4096];for(size_t j=0;j<sizeof(pcm);j+=2){pcm[j]=0xe8;pcm[j+1]=3;}
  for(unsigned i=0;i<8;++i) {
   int rc;do { rc=carplay_audio_sink_packet(102,&f,pcm,sizeof(pcm),1024*i,sink_now_us());
    if(rc==RTCP_ETIMEDOUT)rt_thread_mdelay(1);
   } while(rc==RTCP_ETIMEDOUT);assert(rc==0);
  }
  rt_thread_mdelay(500);
  carplay_audio_sink_stop();host_rtthread_join();check_samples=false;
  assert(output_bytes-before==18*480*4);
 }
 assert(output_bytes>0 && energy>0 && opened==closed);
 /* Sub-10 ms safety sounds remain playable without later packets. */
 assert(carplay_audio_sink_start()==0);
 unsigned before_alert=output_bytes;
 uint8_t alert[96];for(unsigned i=0;i<96;i+=2){alert[i]=0xe8;alert[i+1]=3;}
 assert(carplay_audio_sink_local_pcm(alert,sizeof(alert),48000,1,true)==RTCP_OK);
 rt_thread_mdelay(350);carplay_audio_sink_stop();host_rtthread_join();
 assert(output_bytes-before_alert==480*4);
 puts("actual RT-Thread audio sink: worker playback, failed startup, selective stop and repeated cleanup passed");
}
