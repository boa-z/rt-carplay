/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_input_lubanlite.h"
#include "rtcp_audio_capture_lubanlite.h"
#include "rtcp/rtcp.h"
#include "audio_voice.h"
#include "mbedtls/platform_util.h"
#include <rtthread.h>
#include <rtdevice.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    airplay_audio_input_config_t config;
    atomic_bool stop, alive, mute;
    atomic_uint packets, errors, samples;
    rt_thread_t thread;
} input_t;
static input_t input;
bool carplay_audio_input_available(void)
{
    rt_device_t d=rt_device_find("dmic0");
    return d && !d->ref_count && !atomic_load(&input.alive);
}
#if defined(RTCARPLAY_MIC_AEC)
typedef struct { uint64_t at; int16_t pcm[480]; } reference_t;
static reference_t references[32];
static unsigned reference_head;
static rt_mutex_t reference_lock;
static uint64_t input_now_us(void) { return (uint64_t)rt_tick_get()*1000000/RT_TICK_PER_SECOND; }
void carplay_audio_input_reference(const int16_t *stereo,size_t frames)
{
    if(!stereo || frames!=480 || !atomic_load(&input.alive) || !reference_lock) return;
    if(rt_mutex_take(reference_lock,0)!=RT_EOK)return;
    reference_t *r=&references[reference_head++%32];r->at=input_now_us();
    for(unsigned i=0;i<480;++i)r->pcm[i]=(int16_t)(((int32_t)stereo[2*i]+stereo[2*i+1])/2);
    (void)rt_mutex_release(reference_lock);
}
static bool reference_frame(int16_t *out)
{
    uint64_t now=input_now_us();
    uint64_t delay=(uint64_t)RTCARPLAY_MIC_AEC_DELAY_MS*1000;
    if(now<delay || rt_mutex_take(reference_lock,0)!=RT_EOK)return false;
    reference_t *best=NULL;
    for(unsigned i=0;i<32;++i) {
        reference_t *r=&references[i];
        if(r->at && r->at<=now-delay && now-delay-r->at<20000 && (!best || r->at>best->at))best=r;
    }
    if(best)memcpy(out,best->pcm,480*sizeof(*out));
    (void)rt_mutex_release(reference_lock);return best!=NULL;
}
#else
void carplay_audio_input_reference(const int16_t *stereo,size_t frames) { (void)stereo;(void)frames; }
#endif
static void input_thread(void *arg)
{
    input_t *in=arg;
    rtcp_audio_capture_t *capture=NULL;
    airplay_audio_sender_t *sender=NULL;
    rtcp_audio_voice_t *voice=NULL;
    int fd=-1;
    int rc=rtcp_audio_capture_open(&capture);
    if(rc!=RTCP_OK)goto done;
    rc=airplay_audio_sender_create(&in->config.sender,&sender);
    if(rc!=RTCP_OK)goto done;
    fd=in->config.net->udp_open(in->config.peer.family,0,false);
    if(fd<0) {rc=RTCP_EIO;goto done;}
#if defined(RTCP_AUDIO_CODECS) || defined(LPKG_RT_CARPLAY_AUDIO_CODECS)
    voice=rtcp_audio_voice_create(true,true,
#if defined(RTCARPLAY_MIC_AEC)
                                 true
#else
                                 false
#endif
    );
#else
    voice=rtcp_audio_voice_create(false,true,false);
#endif
    if(!voice) {rc=RTCP_ENOMEM;goto done;}
    unsigned target=in->config.sender.format.sample_rate;
    size_t packet_frames=airplay_audio_sender_frames(sender);
    size_t used=0;
    int16_t frame[960];
    int16_t near_end[480], clean[480]; size_t near_used=0;
    int16_t previous=0;
    uint32_t phase=0;
    rt_kprintf("[carplay-mic] started dmic0=48000/stereo target=%u codec=%u packet=%u\n",
               target,(unsigned)in->config.sender.format.codec,(unsigned)packet_frames);
    while(!atomic_load(&in->stop)) {
        int16_t pcm[480*2];
        int n=rtcp_audio_capture_read(capture,pcm,480);
        if(n<0) {rc=n;break;}
        if(!n) {rt_thread_mdelay(2);continue;}
        for(int j=0;j<n;++j) {
            near_end[near_used++]=(int16_t)(((int32_t)pcm[2*j]+pcm[2*j+1])/2);
            if(near_used!=480)continue;
            near_used=0;
            const int16_t *far=NULL;
#if defined(RTCARPLAY_MIC_AEC)
            int16_t reference[480];if(reference_frame(reference))far=reference;
#endif
            if(rtcp_audio_voice_process(voice,near_end,far,clean)!=RTCP_OK) {rc=RTCP_EIO;break;}
            for(unsigned k=0;k<480;++k) {
                int16_t mono=clean[k];
                phase+=target;
                if(phase>=48000) {
                    phase-=48000;
                    frame[used++]=(int16_t)(previous+((int64_t)mono-previous)*(target-phase)/target);
                    if(used==packet_frames) {
                        uint8_t wire[1452];
                        int bytes=airplay_audio_sender_encode(sender,frame,used,atomic_load(&in->mute),wire,sizeof(wire));
                        if(bytes<0 || in->config.net->udp_sendto(fd,&in->config.peer,wire,(size_t)bytes)!=bytes)
                            atomic_fetch_add(&in->errors,1);
                        else atomic_fetch_add(&in->packets,1);
                        atomic_fetch_add(&in->samples,(unsigned)used);
                        used=0;
                    }
                }
                previous=mono;
            }
        }
    }

done:
    if(rc!=RTCP_OK) {atomic_fetch_add(&in->errors,1);rt_kprintf("[carplay-mic] source/encoder failure rc=%d\n",rc);}
    if(fd>=0)in->config.net->close(fd);
    airplay_audio_sender_destroy(sender);
    rtcp_audio_voice_destroy(voice);
    rtcp_audio_capture_close(capture);
    mbedtls_platform_zeroize(&in->config,sizeof(in->config));
    atomic_store(&in->alive,false);
}
bool carplay_audio_input_start(const airplay_audio_input_config_t *config)
{
    if(!config || !config->net || !config->net->udp_open || !config->net->udp_sendto || !config->net->close) return false;
    if (atomic_load(&input.alive)) { carplay_audio_input_request_stop(); return false; }
    if(atomic_load(&input.alive) || !carplay_audio_input_available())return false;
    /* Validate packet size/codec before accepting a SETUP; the worker owns its
     * own encoder and device after startup. */
    airplay_audio_sender_t *probe=NULL;
    if(airplay_audio_sender_create(&config->sender,&probe)!=RTCP_OK)return false;
    airplay_audio_sender_destroy(probe);
#if defined(RTCARPLAY_MIC_AEC)
    if(!reference_lock)reference_lock=rt_mutex_create("cmicref",RT_IPC_FLAG_FIFO);
    if(!reference_lock)return false;
    (void)rt_mutex_take(reference_lock,RT_WAITING_FOREVER);
    memset(references,0,sizeof(references));reference_head=0;
    (void)rt_mutex_release(reference_lock);
#endif
    input.config=*config;
    atomic_store(&input.stop,false);atomic_store(&input.alive,true);
    atomic_store(&input.packets,0);atomic_store(&input.errors,0);atomic_store(&input.samples,0);
    input.thread=rt_thread_create("carplay-mic",input_thread,&input,64*1024,17,10);
    if(!input.thread || rt_thread_startup(input.thread)!=RT_EOK) {
        if(input.thread)(void)rt_thread_delete(input.thread);
        mbedtls_platform_zeroize(&input.config,sizeof(input.config));
        atomic_store(&input.alive,false);return false;
    }
    return true;
}
void carplay_audio_input_request_stop(void) { atomic_store(&input.stop,true); }
void carplay_audio_input_stop(void)
{
    atomic_store(&input.stop,true);
    for(unsigned wait=0;atomic_load(&input.alive) && wait<500;++wait)rt_thread_mdelay(10);
    if(atomic_load(&input.alive))rt_kprintf("[carplay-mic] worker busy; resources quarantined\n");
}
void carplay_audio_input_mute(bool mute){atomic_store(&input.mute,mute);}
void carplay_audio_input_status(void)
{
    rt_kprintf("[carplay-mic] running=%u mute=%u packets=%u samples=%u errors=%u\n",
      atomic_load(&input.alive),atomic_load(&input.mute),atomic_load(&input.packets),atomic_load(&input.samples),atomic_load(&input.errors));
}
static void mic_command(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"mute"))carplay_audio_input_mute(true);
    else if(argc==2 && !strcmp(argv[1],"unmute"))carplay_audio_input_mute(false);
    carplay_audio_input_status();
}
MSH_CMD_EXPORT_ALIAS(mic_command,carplay_mic,CarPlay microphone status/mute/unmute);
