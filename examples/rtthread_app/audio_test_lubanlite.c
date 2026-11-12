/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_test_lubanlite.h"
#include "audio_codec.h"
#include "audio_worker_stack.h"
#include "rtcp/rtcp.h"
#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/audio.h>
#include <stdatomic.h>
#include <string.h>
#if defined(RTCARPLAY_MICROPHONE)
#include "rtcp_audio_capture_lubanlite.h"
#endif
#include "audio_test_fixture.h"

static atomic_bool alive, stopping, muted;
static atomic_uint volume=20, frames, bytes, peak_left, peak_right, writes;
static atomic_int last_error;
static audio_test_config_t config;
/* One test worker at a time. Keep caller scratch out of FAAD's stack chain. */
static int16_t test_pcm[2048];

void carplay_audio_test_gain(unsigned percent, bool mute)
{ atomic_store(&volume,percent>100?100:percent);atomic_store(&muted,mute); }
void carplay_audio_test_status(audio_test_status_t *s)
{
    if(!s)return;
    *s=(audio_test_status_t){.running=atomic_load(&alive),.muted=atomic_load(&muted),
      .volume=atomic_load(&volume),.frames=atomic_load(&frames),.bytes=atomic_load(&bytes),
      .peak_left=atomic_load(&peak_left),.peak_right=atomic_load(&peak_right),
      .writes=atomic_load(&writes),.error=atomic_load(&last_error)};
}
static void test_worker(void *arg)
{
    (void)arg;
    rt_device_t dev=NULL;
    rtcp_audio_decoder_t *decoder=NULL;
#if defined(RTCARPLAY_MICROPHONE)
    rtcp_audio_capture_t *capture=NULL;
#endif
    int error=RTCP_OK;
    bool opened=false, started=false;
    bool mic=config.mode==AUDIO_TEST_MIC_LEVEL || config.mode==AUDIO_TEST_MIC_LOOP;
    bool output=config.mode!=AUDIO_TEST_MIC_LEVEL;
    unsigned rate=mic?48000:config.mode==AUDIO_TEST_AAC?44100:config.rate;
    if(mic) {
#if defined(RTCARPLAY_MICROPHONE)
        error=rtcp_audio_capture_open(&capture);
        if(error!=RTCP_OK)goto done;
#else
        error=RTCP_ENOTSUP;goto done;
#endif
    }
    if(config.mode==AUDIO_TEST_AAC) {
        rtcp_audio_format_t f;
        if(!rtcp_audio_format_parse(0x400000,&f)) {error=RTCP_ENOTSUP;goto done;}
        error=rtcp_audio_decoder_create(&f,&decoder);
        if(error!=RTCP_OK)goto done;
    }
    if(output) {
        dev=rt_device_find("sound0");
        if(!dev) {error=RTCP_ENOENT;goto done;}
        if(dev->ref_count) {error=RTCP_ESTATE;goto done;}
        if(rt_device_open(dev,RT_DEVICE_OFLAG_WRONLY)!=RT_EOK) {error=RTCP_EIO;goto done;}
        opened=true;
        struct rt_audio_caps caps={0};caps.main_type=AUDIO_TYPE_OUTPUT;caps.sub_type=AUDIO_DSP_PARAM;
        caps.udata.config.samplerate=rate;caps.udata.config.channels=2;caps.udata.config.samplebits=16;
        if(rt_device_control(dev,AUDIO_CTL_CONFIGURE,&caps)!=RT_EOK) {error=RTCP_EIO;goto done;}
        int stream=AUDIO_STREAM_REPLAY;
        if(rt_device_control(dev,AUDIO_CTL_START,&stream)!=RT_EOK) {error=RTCP_EIO;goto done;}
        started=true;rt_thread_mdelay(200);
    }
    size_t aac_offset=0;
    unsigned phase=0, captured=0;
    /* A sample bound protects output even on a stalled system clock. A wall
     * bound also terminates microphone reads which never return samples. */
    rt_tick_t began=rt_tick_get();
    while(!atomic_load(&stopping) && captured<rate*10U &&
          (rt_tick_t)(rt_tick_get()-began)<rt_tick_from_millisecond(10000)) {
        int16_t *pcm=test_pcm;size_t n=480;
        if(mic) {
#if defined(RTCARPLAY_MICROPHONE)
            int got=rtcp_audio_capture_read(capture,pcm,480);
            if(got<0) {error=got;break;}
            if(!got) {rt_thread_mdelay(2);continue;}
            n=(size_t)got;
#endif
        } else if(decoder) {
            if(aac_offset==sizeof(test_aac))break;
            if(sizeof(test_aac)-aac_offset<7) {error=RTCP_EIO;break;}
            const uint8_t *p=test_aac+aac_offset;
            size_t length=((size_t)(p[3]&3)<<11)|((size_t)p[4]<<3)|(p[5]>>5);
            if(length<7 || length>sizeof(test_aac)-aac_offset) {error=RTCP_EIO;break;}
            size_t samples=0;
            error=rtcp_audio_decoder_decode(decoder,p+7,length-7,pcm,2048,&samples);
            if(error!=RTCP_OK)break;
            aac_offset+=length;n=samples/2;
            if(!n)continue;
        } else {
            for(size_t i=0;i<n;++i) {
                int16_t sample=config.mode==AUDIO_TEST_SILENCE?0:test_sine[(uint64_t)phase*48/rate];
                phase=(phase+1000)%rate;
                pcm[2*i]=pcm[2*i+1]=sample;
            }
        }
        unsigned peaks[2]={0,0};
        unsigned gain=atomic_load(&muted)?0:atomic_load(&volume);
        for(size_t i=0;i<n;++i)for(unsigned c=0;c<2;++c) {
            int sample=pcm[2*i+c];unsigned mag=(unsigned)(sample<0?-sample:sample);
            if(mag>peaks[c])peaks[c]=mag;
            if(config.channel && config.channel!=c+1)sample=0;
            pcm[2*i+c]=(int16_t)(sample*(int)gain/100);
        }
        atomic_store(&peak_left,peaks[0]);atomic_store(&peak_right,peaks[1]);
        atomic_fetch_add(&frames,(unsigned)n);captured+=(unsigned)n;
        if(output) {
            size_t offset=0,total=n*4;
            while(offset<total && !atomic_load(&stopping)) {
                size_t got=rt_device_write(dev,0,(const uint8_t *)pcm+offset,total-offset);
                if(!got || got>total-offset) {error=RTCP_EIO;break;}
                offset+=got;atomic_fetch_add(&bytes,(unsigned)got);atomic_fetch_add(&writes,1);
            }
            if(error!=RTCP_OK)break;
        }
    }
done:
    if(started) {int stream=AUDIO_STREAM_REPLAY;(void)rt_device_control(dev,AUDIO_CTL_STOP,&stream);}
    if(opened)(void)rt_device_close(dev);
#if defined(RTCARPLAY_MICROPHONE)
    rtcp_audio_capture_close(capture);
#endif
    rtcp_audio_decoder_destroy(decoder);
    carplay_audio_stack_report("audio-test");
    atomic_store(&last_error,error);
    rt_kprintf("[audio-test] done mode=%u frames=%u bytes=%u error=%d\n",
               (unsigned)config.mode,atomic_load(&frames),atomic_load(&bytes),error);
    atomic_store(&alive,false); /* final publication, no shared accesses after this */
}
int carplay_audio_test_start(const audio_test_config_t *c)
{
    if(!c || (unsigned)c->mode>AUDIO_TEST_MIC_LOOP || c->channel>2 ||
       (c->rate!=44100 && c->rate!=48000))return RTCP_EINVAL;
    if(atomic_load(&alive))return RTCP_ESTATE;
    config=*c;atomic_store(&stopping,false);atomic_store(&last_error,0);
    atomic_store(&frames,0);atomic_store(&bytes,0);atomic_store(&writes,0);
    atomic_store(&peak_left,0);atomic_store(&peak_right,0);atomic_store(&alive,true);
    rt_thread_t t=rt_thread_create("audio-test",test_worker,NULL,CARPLAY_AUDIO_WORKER_STACK_BYTES,18,10);
    if(!t || rt_thread_startup(t)!=RT_EOK) {
        if(t)rt_thread_delete(t);
        atomic_store(&alive,false);atomic_store(&last_error,RTCP_ENOMEM);return RTCP_ENOMEM;
    }
    return RTCP_OK;
}
int carplay_audio_test_stop(void)
{
    atomic_store(&stopping,true);
    for(unsigned i=0;i<200 && atomic_load(&alive);++i)rt_thread_mdelay(10);
    return atomic_load(&alive)?RTCP_ETIMEDOUT:RTCP_OK;
}
