/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "audio_codec.h"
#include "audio_voice.h"
#include "audio_mixer.h"
#include "audio_jitter.h"
#include "opus.h"
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* Track allocations made by linked project/codec objects, excluding libc's
 * internal allocator. Alignment and realloc semantics are preserved. */
typedef union { max_align_t alignment; size_t bytes; } allocation_t;
void *__real_malloc(size_t);void __real_free(void *);
static size_t live,peak;
void *__wrap_malloc(size_t n) {
    if(n>SIZE_MAX-sizeof(allocation_t))return NULL;
    allocation_t *a=__real_malloc(sizeof(*a)+n);if(!a)return NULL;
    a->bytes=n;live+=n;if(live>peak)peak=live;return a+1;
}
void __wrap_free(void *p) {if(p){allocation_t *a=(allocation_t *)p-1;live-=a->bytes;__real_free(a);}}
void *__wrap_calloc(size_t n,size_t s) {
    if(s && n>SIZE_MAX/s)return NULL;
    void *p=__wrap_malloc(n*s);if(p)memset(p,0,n*s);return p;
}
void *__wrap_realloc(void *p,size_t n) {
    if(!p)return __wrap_malloc(n);
    if(!n){__wrap_free(p);return NULL;}
    void *q=__wrap_malloc(n);if(q){size_t old=((allocation_t *)p-1)->bytes;memcpy(q,p,n<old?n:old);__wrap_free(p);}return q;
}
#define RUNS 300U
static uint64_t now(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000U+(uint64_t)t.tv_nsec;}
static int compare(const void *a,const void *b){uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return x<y?-1:x>y;}
static void report(const char *label,uint64_t t[RUNS],double duration_ms) {
    uint64_t sum=0;for(unsigned i=0;i<RUNS;++i)sum+=t[i];qsort(t,RUNS,sizeof(*t),compare);
    printf("%s p50_us=%.2f p95_us=%.2f max_us=%.2f host_core_percent=%.3f heap_peak_bytes=%zu\n",label,
      t[RUNS/2]/1000.,t[RUNS*95/100]/1000.,t[RUNS-1]/1000.,sum/(RUNS*duration_ms*10000.),peak);
}
static void aac(void) {
    FILE *f=fopen("tests/fixtures/audio/sine-48k.aac","rb");assert(f);
    uint8_t bytes[8192];size_t n=fread(bytes,1,sizeof(bytes),f);fclose(f);
    const uint8_t *packets[32];size_t sizes[32],count=0;
    for(size_t off=0;off<n;){assert(n-off>=7 && count<32);size_t len=((bytes[off+3]&3U)<<11)|((size_t)bytes[off+4]<<3)|(bytes[off+5]>>5);assert(len>=7 && len<=n-off);packets[count]=bytes+off+7;sizes[count++]=len-7;off+=len;}
    peak=live;rtcp_audio_format_t fmt;assert(rtcp_audio_format_parse(0x800000,&fmt));rtcp_audio_decoder_t *d;
    assert(!rtcp_audio_decoder_create(&fmt,&d));int16_t pcm[2048];uint64_t t[RUNS];
    for(unsigned i=0;i<RUNS;++i){size_t samples;uint64_t start=now();assert(!rtcp_audio_decoder_decode(d,packets[i%count],sizes[i%count],pcm,2048,&samples));t[i]=now()-start;}
    report("AAC-LC-48k-stereo-decode",t,1024./48);rtcp_audio_decoder_destroy(d);assert(!live);
}
static void opus(void) {
    peak=live;int err;OpusEncoder *e=opus_encoder_create(48000,1,OPUS_APPLICATION_VOIP,&err);assert(e && !err);
    rtcp_audio_format_t f;assert(rtcp_audio_format_parse(0x40000000,&f));rtcp_audio_decoder_t *d;assert(!rtcp_audio_decoder_create(&f,&d));
    int16_t pcm[960],out[960];for(unsigned i=0;i<960;++i)pcm[i]=(int16_t)((i%48)*200-4800);
    uint8_t packet[1452];uint64_t encode[RUNS],decode[RUNS];
    for(unsigned i=0;i<RUNS;++i){uint64_t start=now();int n=opus_encode(e,pcm,960,packet,sizeof(packet));assert(n>0);encode[i]=now()-start;size_t samples;start=now();assert(!rtcp_audio_decoder_decode(d,packet,(size_t)n,out,960,&samples)&&samples==960);decode[i]=now()-start;}
    report("Opus-48k-mono-encode",encode,20);report("Opus-48k-mono-decode",decode,20);
    rtcp_audio_decoder_destroy(d);opus_encoder_destroy(e);assert(!live);
}
static void dsp(bool aec) {
    peak=live;rtcp_audio_voice_t *v=rtcp_audio_voice_create(true,true,aec);assert(v);
    int16_t in[480],far[480],out[480];for(unsigned i=0;i<480;++i)in[i]=far[i]=(int16_t)((i%48)*100-2400);
    uint64_t t[RUNS];for(unsigned i=0;i<RUNS;++i){uint64_t start=now();assert(!rtcp_audio_voice_process(v,in,aec?far:NULL,out));t[i]=now()-start;}
    report(aec?"NS-AGC-AEC-48k-mono":"NS-AGC-48k-mono",t,10);rtcp_audio_voice_destroy(v);assert(!live);
}
static void mix(void) {
    peak=live;rtcp_audio_mixer_t *m=rtcp_audio_mixer_create(8192);assert(m);
    rtcp_audio_jitter_t *q[9];for(unsigned i=0;i<9;++i){q[i]=rtcp_audio_jitter_create(i==8?4:16,20000);assert(q[i]);}
    int16_t in[960]={0},out[960];uint64_t t[RUNS];
    for(unsigned i=0;i<RUNS;++i){uint64_t start=now();for(unsigned j=0;j<9;++j)assert(!rtcp_audio_mixer_push(m,j,in,480,48000,2));assert(rtcp_audio_mixer_render(m,out,480)==480);t[i]=now()-start;}
    report("9-lane-mix-plus-allocated-jitter",t,10);
    rtcp_audio_mixer_destroy(m);for(unsigned i=0;i<9;++i)rtcp_audio_jitter_destroy(q[i]);assert(!live);
}
int main(void){puts("Host optimized single-thread benchmark; not D213 timing. Heap excludes libc, thread stacks and device DMA.");aac();opus();dsp(false);dsp(true);mix();return 0;}
