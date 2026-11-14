/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_audio_sender.h"
#include "rtcp/rtcp.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/platform_util.h"
#include <stdlib.h>
#include <string.h>
#ifdef RTCP_AUDIO_CODECS
#include "opus.h"
#endif
#define UPLINK_MTU_BYTES 1452U
struct airplay_audio_sender {
    mbedtls_chachapoly_context cipher;
    rtcp_audio_format_t format;
    unsigned frames;
    uint16_t sequence;
    uint32_t timestamp;
    uint64_t nonce;
    bool exhausted;
    uint8_t payload_type;
#ifdef RTCP_AUDIO_CODECS
    OpusEncoder *opus;
#endif
};
static void be32(uint8_t *p,uint32_t v) {p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
int airplay_audio_sender_create(const airplay_audio_sender_config_t *c, airplay_audio_sender_t **out)
{
    if (!out) return RTCP_EINVAL;
    *out=NULL;
    if (!c || c->format.channels!=1 || c->format.sample_rate<8000 || c->format.sample_rate>48000 ||
        !c->format.clock_rate || c->payload_type>127 || c->format.codec==RTCP_AUDIO_AAC_LC) return RTCP_ENOTSUP;
    airplay_audio_sender_t *s=calloc(1,sizeof(*s)); if(!s)return RTCP_ENOMEM;
    s->format=c->format;s->payload_type=c->payload_type;
    s->frames=c->frames_per_packet;
    mbedtls_chachapoly_init(&s->cipher);
    if(mbedtls_chachapoly_setkey(&s->cipher,c->input_key)) {airplay_audio_sender_destroy(s);return RTCP_EIO;}
    if (s->format.codec==RTCP_AUDIO_LPCM) {
        if(!s->frames) {
            s->frames=s->format.sample_rate/50;
            if (s->frames*2+36>UPLINK_MTU_BYTES) s->frames=s->format.sample_rate/100;
        }
        if(!s->frames || s->frames>960 || s->frames*2+36>UPLINK_MTU_BYTES || s->format.clock_rate!=s->format.sample_rate) goto unsupported;
    } else {
#ifdef RTCP_AUDIO_CODECS
        if(s->format.codec!=RTCP_AUDIO_OPUS || s->format.sample_rate!=48000 ||
           (s->format.clock_rate!=16000 && s->format.clock_rate!=24000 && s->format.clock_rate!=48000)) goto unsupported;
        if(s->frames && s->frames!=960) goto unsupported;
        s->frames=960;int err;
        s->opus=opus_encoder_create(48000,1,OPUS_APPLICATION_VOIP,&err);
        if(!s->opus || err!=OPUS_OK) goto unsupported;
        int bitrate=s->format.clock_rate==16000?48000:s->format.clock_rate==24000?64000:96000;
        if(opus_encoder_ctl(s->opus,OPUS_SET_BITRATE(bitrate))!=OPUS_OK ||
           opus_encoder_ctl(s->opus,OPUS_SET_COMPLEXITY(5))!=OPUS_OK) goto unsupported;
#else
        goto unsupported;
#endif
    }
    *out=s;return RTCP_OK;
unsupported:
    airplay_audio_sender_destroy(s);return RTCP_ENOTSUP;
}
size_t airplay_audio_sender_frames(const airplay_audio_sender_t *s){return s?s->frames:0;}
int airplay_audio_sender_encode(airplay_audio_sender_t *s,const int16_t *pcm,size_t frames,bool mute,uint8_t *wire,size_t capacity)
{
    if(!s || !pcm || !wire || frames!=s->frames || capacity<UPLINK_MTU_BYTES || s->exhausted) return RTCP_EINVAL;
    uint8_t body[UPLINK_MTU_BYTES];size_t len;
    if(s->format.codec==RTCP_AUDIO_LPCM) {
        len=frames*2;
        for(size_t i=0;i<frames;++i) {uint16_t v=mute?0:(uint16_t)pcm[i];body[2*i]=(uint8_t)(v>>8);body[2*i+1]=(uint8_t)v;}
    } else {
#ifdef RTCP_AUDIO_CODECS
        int16_t silence[960]={0};
        int n=opus_encode(s->opus,mute?silence:pcm,(int)frames,body,sizeof(body)-36);
        if(n<0)return RTCP_EIO;
        len=(size_t)n;
#else
        return RTCP_ENOTSUP;
#endif
    }
    wire[0]=0x80;wire[1]=s->payload_type;wire[2]=(uint8_t)(s->sequence>>8);wire[3]=(uint8_t)s->sequence;
    be32(wire+4,s->timestamp);be32(wire+8,0);
    uint8_t nonce[12]={0};
    for(unsigned i=0;i<8;++i)nonce[4+i]=(uint8_t)(s->nonce>>(8*i));
    int rc=mbedtls_chachapoly_encrypt_and_tag(&s->cipher,len,nonce,wire+4,8,body,wire+12,wire+12+len);
    /* Conservatively retire the nonce even on an encryption error. */
    if(s->nonce==UINT64_MAX)s->exhausted=true;else ++s->nonce;
    ++s->sequence;s->timestamp+=(uint32_t)((uint64_t)frames*s->format.clock_rate/s->format.sample_rate);
    mbedtls_platform_zeroize(body,sizeof(body));
    if(rc)return RTCP_EIO;
    memcpy(wire+12+len+16,nonce+4,8);return (int)(len+36);
}
void airplay_audio_sender_destroy(airplay_audio_sender_t *s)
{
    if(!s)return;
#ifdef RTCP_AUDIO_CODECS
    if(s->opus)opus_encoder_destroy(s->opus);
#endif
    mbedtls_chachapoly_free(&s->cipher);mbedtls_platform_zeroize(s,sizeof(*s));free(s);
}
