/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_codec.h"
#include "audio_jitter.h"
#include "audio_mixer.h"
#include "rtcp/rtcp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef RTCP_AUDIO_CODECS
#include "opus.h"
#endif
static void jitter(void)
{
    rtcp_audio_jitter_t *q = rtcp_audio_jitter_create(3,20000);
    assert(q); uint8_t p[2]={1,2}, out[2]; uint32_t ts;
    assert(rtcp_audio_jitter_push(q,p,2,UINT32_MAX-99,1000)==0);
    assert(rtcp_audio_jitter_push(q,p,2,100,2000)==0);
    assert(rtcp_audio_jitter_push(q,p,2,0,3000)==0);
    assert(rtcp_audio_jitter_push(q,p,2,0,3000)==RTCP_EINVAL);
    assert(rtcp_audio_jitter_push(q,p,2,200,3000)==RTCP_ENOMEM);
    assert(rtcp_audio_jitter_pop(q,out,2,&ts,20999)==0);
    assert(rtcp_audio_jitter_pop(q,out,1,&ts,21000)==RTCP_EINVAL);
    assert(rtcp_audio_jitter_pop(q,out,2,&ts,21000)==2 && ts==UINT32_MAX-99);
    assert(rtcp_audio_jitter_pop(q,out,2,&ts,23000)==2 && ts==0);
    assert(rtcp_audio_jitter_push(q,p,2,UINT32_MAX-99,24000)==RTCP_EINVAL);
    assert(rtcp_audio_jitter_pop(q,out,2,&ts,23000)==2 && ts==100);
    rtcp_audio_jitter_stats_t stats; rtcp_audio_jitter_stats(q,&stats);
    assert(stats.accepted==3 && stats.duplicates==1 && stats.late==1 && stats.overflow==1 && !stats.queued);
    rtcp_audio_jitter_flush(q); assert(rtcp_audio_jitter_push(q,p,2,0,0)==0);
    rtcp_audio_jitter_destroy(q);
}
static void mixing(void)
{
    rtcp_audio_mixer_t *m=rtcp_audio_mixer_create(2048); assert(m);
    int16_t a[960], b[960], out[960*2];
    for(unsigned i=0;i<960;++i) { a[i]=12000; b[i]=4000; }
    assert(rtcp_audio_mixer_push(m,2,a,960,48000,1)==0);
    assert(rtcp_audio_mixer_push(m,1,b,960,48000,1)==0);
    assert(rtcp_audio_mixer_render(m,out,480)==480);
    assert(out[0]==7000 && out[1]==7000); /* media/4 + navigation */
    rtcp_audio_mixer_flush(m,1);
    assert(rtcp_audio_mixer_render(m,out,480)==480 && out[0]==12000);
    rtcp_audio_mixer_master(m,0,true);
    assert(rtcp_audio_mixer_push(m,3,b,960,48000,1)==0);
    assert(rtcp_audio_mixer_render(m,out,480)==480 && out[0]==4000);
    rtcp_audio_mixer_flush(m,3); rtcp_audio_mixer_master(m,32768,false);
    rtcp_audio_mixer_gain(m,0,0,4);
    assert(rtcp_audio_mixer_push(m,0,a,960,24000,1)==0);
    assert(rtcp_audio_mixer_render(m,out,4)==4);
    assert(out[0]==9000 && out[2]==6000 && out[4]==3000 && out[6]==0);
    rtcp_audio_lane_stats_t stats; rtcp_audio_mixer_stats(m,0,&stats);
    assert(stats.queued_frames==958); /* exact 24->48 kHz ratio */
    rtcp_audio_mixer_flush(m,0); rtcp_audio_mixer_gain(m,0,32768,0);
    for(unsigned i=0;i<960;++i) a[i]=b[i]=30000;
    assert(rtcp_audio_mixer_push(m,0,a,960,48000,1)==0);
    assert(rtcp_audio_mixer_push(m,1,b,960,48000,1)==0);
    assert(rtcp_audio_mixer_render(m,out,480)==480 && out[0]==32767);
    rtcp_audio_mixer_destroy(m);
}
#ifdef RTCP_AUDIO_CODECS
static void aac(const char *path,uint64_t bits)
{
    FILE *file=fopen(path,"rb"); assert(file);
    uint8_t input[8192]; size_t total=fread(input,1,sizeof(input),file); fclose(file);
    rtcp_audio_format_t f; assert(rtcp_audio_format_parse(bits,&f));
    rtcp_audio_decoder_t *d; assert(rtcp_audio_decoder_create(&f,&d)==0);
    unsigned frames=0; uint64_t energy=0;
    for(size_t off=0;off<total;) {
        assert(total-off>=7 && input[off]==0xff && (input[off+1]&0xf6)==0xf0);
        size_t len=((input[off+3]&3U)<<11)|((size_t)input[off+4]<<3)|(input[off+5]>>5);
        assert(len>=7 && len<=total-off);
        int16_t pcm[2048]; size_t n=0;
        assert(rtcp_audio_decoder_decode(d,input+off+7,len-7,pcm,2048,&n)==0);
        assert(n==0 || n==2048);
        for(size_t i=0;i<n;++i) energy+=(uint64_t)((int64_t)pcm[i]*pcm[i]);
        off+=len; ++frames;
    }
    assert(frames>=5 && energy>1000000); rtcp_audio_decoder_destroy(d);
}
static void opus(void)
{
    int err; OpusEncoder *e=opus_encoder_create(48000,1,OPUS_APPLICATION_VOIP,&err);
    assert(e && !err);
    int16_t pcm[960]; for(unsigned i=0;i<960;++i) pcm[i]=(int16_t)((i%48)*200-4800);
    uint8_t packet[4096]; int len=opus_encode(e,pcm,960,packet,sizeof(packet)); assert(len>0);
    rtcp_audio_format_t f; assert(rtcp_audio_format_parse(0x20000000,&f));
    rtcp_audio_decoder_t *d; assert(rtcp_audio_decoder_create(&f,&d)==0);
    size_t n=0; assert(rtcp_audio_decoder_decode(d,packet,(size_t)len,pcm,960,&n)==0 && n==960);
    assert(rtcp_audio_decoder_decode(d,packet,(size_t)len,pcm,100,&n)!=0 && n==0);
    rtcp_audio_decoder_destroy(d); opus_encoder_destroy(e);
}
#endif
int main(void)
{
    jitter(); mixing();
#ifdef RTCP_AUDIO_CODECS
    aac("tests/fixtures/audio/sine-48k.aac",0x800000);
    aac("tests/fixtures/audio/sine-44k.aac",0x400000); opus();
#endif
    puts("audio engine: real AAC/Opus decode, jitter, isolation, mixing, safety, gain and resampling passed");
    return 0;
}
