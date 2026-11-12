/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../examples/rtthread_app/audio_test_lubanlite.c"
static struct host_audio_device speaker;
static unsigned time_ms, total_frames, peak[2], open_count, close_count;
static unsigned configured_rate, controls, startup_fail, write_fail, configure_fail;
static void (*entry)(void *);
static bool capture_open;
rt_tick_t rt_tick_get(void) { return time_ms; }
rt_tick_t rt_tick_from_millisecond(int n) { return (unsigned)n; }
void rt_thread_mdelay(int n) { time_ms+=(unsigned)n; }
int rt_kprintf(const char *f,...) {(void)f;return 0;}
rt_thread_t rt_thread_create(const char *name,void (*fn)(void *),void *arg,unsigned stack,unsigned prio,unsigned tick)
{(void)name;(void)arg;(void)stack;(void)prio;(void)tick;entry=fn;return (void *)1;}
int rt_thread_startup(rt_thread_t t) {(void)t;return startup_fail?-1:0;}
int rt_thread_delete(rt_thread_t t) {(void)t;return 0;}
rt_device_t rt_device_find(const char *name) {assert(!strcmp(name,"sound0"));return &speaker;}
int rt_device_open(rt_device_t d,int flags) {assert(d==&speaker && flags==RT_DEVICE_OFLAG_WRONLY);++open_count;++d->ref_count;return 0;}
int rt_device_close(rt_device_t d) {assert(d->ref_count==1);--d->ref_count;++close_count;return 0;}
int rt_device_control(rt_device_t d,int cmd,void *p) {
 assert(d==&speaker);++controls;
 if(cmd==AUDIO_CTL_CONFIGURE) {
  struct rt_audio_caps *caps=p;
  assert(caps->udata.config.channels==2 && caps->udata.config.samplebits==16);
  configured_rate=caps->udata.config.samplerate;
  if(configure_fail)return -1;
 }
 return 0;
}
rt_size_t rt_device_write(rt_device_t d,int pos,const void *data,rt_size_t len) {
 (void)pos;assert(d==&speaker && len%4==0);
 if(write_fail)return 0;
 const int16_t *pcm=data;
 for(size_t i=0;i<len/2;++i){int v=pcm[i];unsigned mag=(unsigned)(v<0?-v:v);if(mag>peak[i%2])peak[i%2]=mag;}
 total_frames+=(unsigned)len/4;
 time_ms+=(unsigned)(len/4*1000/configured_rate);
 return len;
}
int rtcp_audio_capture_open(rtcp_audio_capture_t **out) {assert(!capture_open);capture_open=true;*out=(void *)2;return 0;}
int rtcp_audio_capture_read(rtcp_audio_capture_t *c,int16_t *pcm,size_t n) {
 assert(c==(void *)2 && n==480);time_ms+=10;
 for(size_t i=0;i<n;++i){pcm[2*i]=1000;pcm[2*i+1]=-2000;}
 return (int)n;
}
void rtcp_audio_capture_close(rtcp_audio_capture_t *c) {if(c){assert(capture_open);capture_open=false;}}
static void run(audio_test_mode_t mode,unsigned rate,unsigned channel,unsigned gain,bool mute)
{
 audio_test_config_t c={mode,rate,channel};carplay_audio_test_gain(gain,mute);
 total_frames=peak[0]=peak[1]=0;time_ms=0;
 assert(carplay_audio_test_start(&c)==0);
 assert(carplay_audio_test_start(&c)==RTCP_ESTATE);
 entry(NULL);
 audio_test_status_t s;carplay_audio_test_status(&s);
 assert(!s.running && !s.error && !speaker.ref_count && !capture_open);
 assert(s.frames>0 && (mode==AUDIO_TEST_MIC_LEVEL?s.bytes==0:s.bytes==total_frames*4));
}
int main(void)
{
 run(AUDIO_TEST_TONE,48000,0,100,false);assert(peak[0]==4096 && peak[1]==4096 && total_frames==480000);
 run(AUDIO_TEST_TONE,44100,1,50,false);assert(peak[0]>2000 && peak[1]==0 && configured_rate==44100);
 run(AUDIO_TEST_TONE,48000,2,100,false);assert(peak[0]==0 && peak[1]==4096);
 run(AUDIO_TEST_SILENCE,48000,0,100,false);assert(!peak[0] && !peak[1]);
 run(AUDIO_TEST_TONE,48000,0,100,true);assert(!peak[0] && !peak[1]);
#ifdef RTCP_AUDIO_CODECS
 run(AUDIO_TEST_AAC,48000,0,100,false);assert(peak[0]>0 && configured_rate==44100);
#endif
 run(AUDIO_TEST_MIC_LEVEL,48000,0,20,false);audio_test_status_t s;carplay_audio_test_status(&s);
 assert(s.peak_left==1000 && s.peak_right==2000 && !s.bytes);
 run(AUDIO_TEST_MIC_LOOP,48000,0,20,false);assert(peak[0]==200 && peak[1]==400);
 audio_test_config_t c={AUDIO_TEST_TONE,48000,0};
 assert(carplay_audio_test_start(&c)==0);assert(carplay_audio_test_stop()==RTCP_ETIMEDOUT);entry(NULL);
 carplay_audio_test_status(&s);assert(!s.running && !s.bytes && !s.frames);
 speaker.ref_count=1;assert(carplay_audio_test_start(&c)==0);entry(NULL);carplay_audio_test_status(&s);assert(s.error==RTCP_ESTATE);speaker.ref_count=0;
 write_fail=1;assert(carplay_audio_test_start(&c)==0);entry(NULL);carplay_audio_test_status(&s);assert(s.error==RTCP_EIO);write_fail=0;
 configure_fail=1;assert(carplay_audio_test_start(&c)==0);entry(NULL);carplay_audio_test_status(&s);assert(s.error==RTCP_EIO);configure_fail=0;
 startup_fail=1;assert(carplay_audio_test_start(&c)==RTCP_ENOMEM);
 assert(open_count==close_count);
 puts("offline audio worker: tone/silence/AAC, rates/channels/gain/mute, microphone meter/loopback, busy/failure/cleanup passed");
}
