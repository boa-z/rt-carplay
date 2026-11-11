/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_codec.h"
#include "audio_voice.h"
#include "audio_focus.h"
#include "airplay_audio_sender.h"
#include "mbedtls/chachapoly.h"
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
    assert(rtcp_audio_mixer_push(m,RTCP_AUDIO_LOCAL_LANE,b,960,48000,1)==0);
    assert(rtcp_audio_mixer_render(m,out,480)==480 && out[0]==4000);
    rtcp_audio_mixer_flush(m,RTCP_AUDIO_LOCAL_LANE); rtcp_audio_mixer_master(m,32768,false);
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
/* Simulate a source that gains/loses 100 ppm against the output clock.
 * The bounded servo must retain occupancy without drops over 120 seconds. */
static void drift(void)
{
    for (int direction=-1; direction<=1; direction+=2) {
        rtcp_audio_mixer_t *m=rtcp_audio_mixer_create(4096); assert(m);
        int16_t in[2048]={0}, out[960];
        assert(rtcp_audio_mixer_push(m,0,in,960,48000,1)==0);
        rtcp_audio_mixer_clock(m,0,1440);
        unsigned fractional=0;
        for (unsigned tick=0; tick<12000; ++tick) {
            unsigned n=480; fractional+=480;
            if (fractional>=10000) { n=(unsigned)((int)n+direction);fractional-=10000; }
            assert(rtcp_audio_mixer_push(m,0,in,n,48000,1)==0);
            assert(rtcp_audio_mixer_render(m,out,480)==480);
        }
        rtcp_audio_lane_stats_t st;rtcp_audio_mixer_stats(m,0,&st);
        assert(!st.dropped_frames && !st.underrun_frames);
        assert(st.queued_frames>600 && st.queued_frames<1400);
        assert(st.trim_ppm*direction>0 && abs(st.trim_ppm)<=300);
        rtcp_audio_mixer_destroy(m);
    }
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
static void focus(void)
{
    rtcp_audio_focus_t f={0};rtcp_audio_focus_policy_t p;
    rtcp_audio_focus_policy(&f,&p);assert(p.media_gain==32768);
    f.navigation_active=true;rtcp_audio_focus_policy(&f,&p);assert(p.media_gain==8192 && p.main_gain==32768);
    f.call_active=true;rtcp_audio_focus_policy(&f,&p);assert(p.media_gain==0 && p.main_gain==32768);
    f.call_active=false;f.siri_active=true;rtcp_audio_focus_policy(&f,&p);assert(p.media_gain==0);
    f.critical_active=true;rtcp_audio_focus_policy(&f,&p);assert(p.main_gain==0 && p.alternate_gain==0 && p.local_gain==32768);
}
static void voice(void)
{
    rtcp_audio_voice_t *v=rtcp_audio_voice_create(false,true,false);assert(v);
    int16_t near_end[480], out[480];
    for(unsigned i=0;i<480;++i)near_end[i]=1000;
    for(unsigned i=0;i<200;++i)assert(rtcp_audio_voice_process(v,near_end,NULL,out)==0);
    assert(out[0]>3000 && out[0]<4100);
    memset(near_end,0,sizeof(near_end));assert(rtcp_audio_voice_process(v,near_end,NULL,out)==0);
    for(unsigned i=0;i<480;++i)assert(!out[i]);
    rtcp_audio_voice_destroy(v);
#ifdef RTCP_AUDIO_CODECS
    v=rtcp_audio_voice_create(false,false,true);assert(v);
    int16_t far_end[480];uint32_t rng=1;uint64_t before=0,after=0;
    for(unsigned f=0;f<250;++f) {
        for(unsigned i=0;i<480;++i){rng=rng*1664525U+1013904223U;far_end[i]=(int16_t)((int)(rng>>19)-4096);near_end[i]=far_end[i]/2;}
        assert(rtcp_audio_voice_process(v,near_end,far_end,out)==0);
        if(f>200)for(unsigned i=0;i<480;++i){before+=(uint64_t)((int64_t)near_end[i]*near_end[i]);after+=(uint64_t)((int64_t)out[i]*out[i]);}
    }
    assert(after<before/4); /* synthetic aligned echo attenuation, not a hardware claim */
    rtcp_audio_voice_destroy(v);
    v=rtcp_audio_voice_create(true,false,false);assert(v);
    assert(rtcp_audio_voice_process(v,near_end,NULL,out)==0);rtcp_audio_voice_destroy(v);
#endif
}
static void uplink(void)
{
    airplay_audio_sender_config_t c = {.format={.sample_rate=48000,.channels=1,.clock_rate=48000}, .payload_type=100};
    memset(c.input_key,0x42,32);
    airplay_audio_sender_t *s;
    assert(airplay_audio_sender_create(&c,&s)==0);
    assert(airplay_audio_sender_frames(s)==480); /* IPv6 MTU-safe L16 */
    int16_t pcm[960]; for(unsigned i=0;i<960;++i)pcm[i]=(int16_t)(i*13);
    uint8_t wire[1452], decoded[1452];
    int bytes=airplay_audio_sender_encode(s,pcm,480,false,wire,sizeof(wire));
    assert(bytes==996 && wire[1]==100 && wire[2]==0 && wire[3]==0);
    mbedtls_chachapoly_context cipher;mbedtls_chachapoly_init(&cipher);
    assert(mbedtls_chachapoly_setkey(&cipher,c.input_key)==0);
    uint8_t nonce[12]={0};memcpy(nonce+4,wire+bytes-8,8);
    assert(mbedtls_chachapoly_auth_decrypt(&cipher,bytes-36,nonce,wire+4,8,wire+bytes-24,wire+12,decoded)==0);
    assert(decoded[2]==0 && decoded[3]==13); /* big endian wire L16 */
    bytes=airplay_audio_sender_encode(s,pcm,480,true,wire,sizeof(wire));
    memcpy(nonce+4,wire+bytes-8,8);
    assert(nonce[4]==1 && wire[3]==1 && wire[6]==1 && wire[7]==224); /* clock 480 */
    assert(mbedtls_chachapoly_auth_decrypt(&cipher,bytes-36,nonce,wire+4,8,wire+bytes-24,wire+12,decoded)==0);
    for(int i=0;i<bytes-36;++i)assert(decoded[i]==0);
    airplay_audio_sender_destroy(s);
#ifdef RTCP_AUDIO_CODECS
    assert(rtcp_audio_format_parse(0x20000000,&c.format));
    assert(airplay_audio_sender_create(&c,&s)==0);
    assert(airplay_audio_sender_frames(s)==960);
    bytes=airplay_audio_sender_encode(s,pcm,960,false,wire,sizeof(wire));assert(bytes>36 && bytes<=1452);
    memcpy(nonce+4,wire+bytes-8,8);
    assert(mbedtls_chachapoly_auth_decrypt(&cipher,bytes-36,nonce,wire+4,8,wire+bytes-24,wire+12,decoded)==0);
    rtcp_audio_decoder_t *d;assert(rtcp_audio_decoder_create(&c.format,&d)==0);
    size_t n;assert(rtcp_audio_decoder_decode(d,decoded,(size_t)bytes-36,pcm,960,&n)==0 && n==960);
    bytes=airplay_audio_sender_encode(s,pcm,960,false,wire,sizeof(wire));assert(bytes>36);
    assert(wire[6]==1 && wire[7]==224); /* 24k RTP clock, 48k capture */
    rtcp_audio_decoder_destroy(d);airplay_audio_sender_destroy(s);
#endif
    mbedtls_chachapoly_free(&cipher);
}
int main(void)
{
    jitter(); mixing(); drift(); uplink(); voice(); focus();
#ifdef RTCP_AUDIO_CODECS
    aac("tests/fixtures/audio/sine-48k.aac",0x800000);
    aac("tests/fixtures/audio/sine-44k.aac",0x400000); opus();
#endif
    puts("audio engine: real AAC/Opus decode, jitter, isolation, mixing, safety, gain and resampling passed");
    return 0;
}
