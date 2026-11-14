/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "include/audio_voice.h"
#include "rtcp/rtcp.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef RTCP_AUDIO_CODECS
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#endif
struct rtcp_audio_voice {
    bool agc;
    float gain;
#ifdef RTCP_AUDIO_CODECS
    SpeexPreprocessState *pre;
    SpeexEchoState *echo;
#endif
};
rtcp_audio_voice_t *rtcp_audio_voice_create(bool ns,bool agc,bool aec)
{
#ifndef RTCP_AUDIO_CODECS
    if(ns || aec)return NULL;
#endif
    rtcp_audio_voice_t *v=calloc(1,sizeof(*v));if(!v)return NULL;
    v->agc=agc;v->gain=1;
#ifdef RTCP_AUDIO_CODECS
    if(aec) {
        v->echo=speex_echo_state_init(RTCP_VOICE_FRAME,4800);if(!v->echo)goto fail;
        int rate=48000;if(speex_echo_ctl(v->echo,SPEEX_ECHO_SET_SAMPLING_RATE,&rate))goto fail;
    }
    if(ns) {
        v->pre=speex_preprocess_state_init(RTCP_VOICE_FRAME,48000);if(!v->pre)goto fail;
        int yes=1,noise=-18;
        if(speex_preprocess_ctl(v->pre,SPEEX_PREPROCESS_SET_DENOISE,&yes) ||
           speex_preprocess_ctl(v->pre,SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,&noise))goto fail;
        if(v->echo && speex_preprocess_ctl(v->pre,SPEEX_PREPROCESS_SET_ECHO_STATE,v->echo))goto fail;
    }
#endif
    return v;
#ifdef RTCP_AUDIO_CODECS
fail:
    rtcp_audio_voice_destroy(v);return NULL;
#endif
}
int rtcp_audio_voice_process(rtcp_audio_voice_t *v,const int16_t near_end[RTCP_VOICE_FRAME],const int16_t *far_end,int16_t out[RTCP_VOICE_FRAME])
{
    if(!v || !near_end || !out)return RTCP_EINVAL;
#ifdef RTCP_AUDIO_CODECS
    if(v->echo && far_end)speex_echo_cancellation(v->echo,near_end,far_end,out);
    else memcpy(out,near_end,RTCP_VOICE_FRAME*sizeof(*out));
    if(v->pre)(void)speex_preprocess_run(v->pre,out);
#else
    (void)far_end;memcpy(out,near_end,RTCP_VOICE_FRAME*sizeof(*out));
#endif
    if(v->agc) {
        double energy=0;
        for(unsigned i=0;i<RTCP_VOICE_FRAME;++i)energy+=(double)out[i]*out[i];
        float rms=sqrtf((float)(energy/RTCP_VOICE_FRAME));
        /* Below -50 dBFS do not amplify background/silence. */
        float desired=rms<100 ? 1 : 4000/rms;
        if(desired>4)desired=4;
        if(desired<0.125f)desired=0.125f;
        v->gain+=(desired-v->gain)*(desired<v->gain ? 0.25f : 0.01f);
        for(unsigned i=0;i<RTCP_VOICE_FRAME;++i) {
            float sample=out[i]*v->gain;
            out[i]=(int16_t)(sample>32767?32767:sample< -32768?-32768:sample);
        }
    }
    return RTCP_OK;
}
void rtcp_audio_voice_destroy(rtcp_audio_voice_t *v)
{
    if(!v)return;
#ifdef RTCP_AUDIO_CODECS
    if(v->pre)speex_preprocess_state_destroy(v->pre);
    if(v->echo)speex_echo_state_destroy(v->echo);
#endif
    free(v);
}
