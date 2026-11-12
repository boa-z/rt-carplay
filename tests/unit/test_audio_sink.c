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
static atomic_bool allow_native;
static atomic_uint actual_rate;
static atomic_uint mixed_bytes;
rt_device_t rt_device_find(const char *n) { assert(!strcmp(n,"sound0"));return (void *)1; }
int rt_device_open(rt_device_t d,int f) {(void)d;(void)f;++opened;return 0;}
int rt_device_close(rt_device_t d) {(void)d;++closed;return 0;}
int rt_device_control(rt_device_t d,int cmd,void *a) {
 (void)d;if(cmd==AUDIO_CTL_CONFIGURE) {struct rt_audio_caps *c=a;assert((c->udata.config.samplerate==48000 || (allow_native && c->udata.config.samplerate==44100)) && c->udata.config.channels==2 && c->udata.config.samplebits==16);actual_rate=c->udata.config.samplerate;} return 0;
}
rt_size_t rt_device_write(rt_device_t d,int p,const void *data,rt_size_t n) {
 (void)d;(void)p;assert(n%4==0);const int16_t *pcm=data;
 for(size_t i=0;i<n/2;++i) { if(check_samples && pcm[i]!=expected_sample) {fprintf(stderr,"sample=%d expected=%d offset=%zu\n",pcm[i],(int)expected_sample,i);assert(0); } }
 for(size_t i=0;i<n/2;++i) atomic_fetch_add(&energy,(unsigned long long)((int64_t)pcm[i]*pcm[i]));
 output_bytes+=(unsigned)n;if(actual_rate==48000)mixed_bytes+=(unsigned)n;rt_thread_mdelay(5);return n;
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
 /* Direct media preserves source frames/rate and gain, then switches back. */
 allow_native=true;
 assert(carplay_audio_sink_start()==0);
 char *direct_args[]={"carplay_audio_path","direct"};audio_path_command(2,direct_args);
 carplay_audio_sink_set_volume_db(0);
 rtcp_audio_format_t native={.sample_rate=44100,.channels=2,.clock_rate=44100};
 uint8_t native_pcm[4096];for(unsigned i=0;i<4096;i+=2){native_pcm[i]=0xe8;native_pcm[i+1]=3;}
 unsigned before_direct=output_bytes;
 check_samples=true;expected_sample=1000;
 for(unsigned i=0;i<4;++i)assert(carplay_audio_sink_packet(102,&native,native_pcm,sizeof(native_pcm),1024*i,sink_now_us())==0);
 rt_thread_mdelay(450);
 assert(output_bytes-before_direct==4*4096 && actual_rate==44100 && s_sink.direct_active);
 carplay_audio_sink_set_volume_db(-144);expected_sample=0;
 for(unsigned i=4;i<8;++i)assert(carplay_audio_sink_packet(102,&native,native_pcm,sizeof(native_pcm),1024*i,sink_now_us())==0);
 rt_thread_mdelay(450);
 assert(output_bytes-before_direct==8*4096 && s_sink.direct_active);
 carplay_audio_sink_set_volume_db(0);expected_sample=1000;
 char *mixed_args[]={"carplay_audio_path","mixer"};audio_path_command(2,mixed_args);
 for(unsigned i=8;i<12;++i)assert(carplay_audio_sink_packet(102,&native,native_pcm,sizeof(native_pcm),1024*i,sink_now_us())==0);
 rt_thread_mdelay(450);
 assert(actual_rate==48000 && !s_sink.direct_active);
 assert(s_sink.diag[2].negative==0 && s_sink.diag[2].decode_errors==0);
 carplay_audio_sink_stop();host_rtthread_join();check_samples=false;
 /* A requested direct route must yield to a queued critical local sound. */
 assert(carplay_audio_sink_start()==0);audio_path_command(2,direct_args);
 unsigned before_mixed=mixed_bytes;
 assert(carplay_audio_sink_local_pcm(alert,sizeof(alert),48000,1,true)==RTCP_OK);
 for(unsigned i=0;i<4;++i)assert(carplay_audio_sink_packet(102,&native,native_pcm,sizeof(native_pcm),1024*i,sink_now_us())==0);
 rt_thread_mdelay(450);carplay_audio_sink_stop();host_rtthread_join();
 assert(mixed_bytes>before_mixed && s_sink.output_frames>0);
 /* Negative timestamp discards were previously invisible in aggregate errors. */
 assert(carplay_audio_sink_start()==0);
 for(unsigned i=0;i<4;++i)assert(carplay_audio_sink_packet(102,&native,native_pcm,sizeof(native_pcm),512*i,sink_now_us())==0);
 rt_thread_mdelay(400);carplay_audio_sink_stop();host_rtthread_join();
 assert(s_sink.diag[2].negative==2 && s_sink.diag[2].decoded==4 && s_sink.write_errors==0);
#ifdef RTCP_AUDIO_CODECS
 /* Real AAC -> actual board worker -> native-rate output, no mixer/timeline. */
 assert(carplay_audio_sink_start()==0);audio_path_command(2,direct_args);
 assert(rtcp_audio_format_parse(0x400000,&native));
 FILE *file=fopen("tests/fixtures/audio/sine-44k.aac","rb");assert(file);
 uint8_t aac[4096];size_t length=fread(aac,1,sizeof(aac),file);fclose(file);
 uint32_t ts=0;
 for(size_t off=0;off<length;) {
  size_t n=((size_t)(aac[off+3]&3)<<11)|((size_t)aac[off+4]<<3)|(aac[off+5]>>5);
  assert(n>=7 && n<=length-off);
  int rc;do {rc=carplay_audio_sink_packet(102,&native,aac+off+7,n-7,ts,sink_now_us());
   if(rc==RTCP_ETIMEDOUT)rt_thread_mdelay(1);
  } while(rc==RTCP_ETIMEDOUT);assert(rc==0);off+=n;ts+=1024;
 }
 rt_thread_mdelay(500);carplay_audio_sink_stop();host_rtthread_join();
 assert(actual_rate==44100 && s_sink.output_frames==s_sink.diag[2].frames);
 assert(s_sink.diag[2].decoded>=5 && s_sink.diag[2].peak_pcm>0 && !s_sink.diag[2].decode_errors);
#endif
 puts("actual RT-Thread audio sink: worker playback, failed startup, selective stop and repeated cleanup passed");
}
