/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "include/audio_jitter.h"
#include "rtcp/rtcp.h"
#include <stdlib.h>
#include <string.h>
typedef struct { uint8_t data[RTCP_AUDIO_PACKET_MAX]; size_t len; uint32_t ts; uint64_t arrival; } packet_t;
struct rtcp_audio_jitter {
    packet_t *packets;
    size_t capacity, count;
    uint32_t delay, last;
    bool played;
    rtcp_audio_jitter_stats_t stats;
};
rtcp_audio_jitter_t *rtcp_audio_jitter_create(size_t capacity, uint32_t delay)
{
    if (!capacity || capacity > 64 || delay > 200000) return NULL;
    rtcp_audio_jitter_t *q = calloc(1,sizeof(*q));
    if (!q) return NULL;
    q->packets = calloc(capacity,sizeof(packet_t));
    if (!q->packets) { free(q); return NULL; }
    q->capacity = capacity; q->delay = delay; return q;
}
void rtcp_audio_jitter_destroy(rtcp_audio_jitter_t *q) { if (q) { free(q->packets); free(q); } }
void rtcp_audio_jitter_flush(rtcp_audio_jitter_t *q) { if (q) { q->count=0; q->played=false; } }
int rtcp_audio_jitter_push(rtcp_audio_jitter_t *q, const uint8_t *data, size_t len, uint32_t ts, uint64_t arrival)
{
    if (!q || !data || !len || len > RTCP_AUDIO_PACKET_MAX) return RTCP_EINVAL;
    if (q->played && (int32_t)(ts-q->last) <= 0) { ++q->stats.late; return RTCP_EINVAL; }
    size_t pos = 0;
    while (pos < q->count && (int32_t)(q->packets[pos].ts-ts)<0) ++pos;
    if (pos < q->count && q->packets[pos].ts == ts) { ++q->stats.duplicates; return RTCP_EINVAL; }
    if (q->count == q->capacity) { ++q->stats.overflow; return RTCP_ENOMEM; }
    memmove(q->packets+pos+1,q->packets+pos,(q->count-pos)*sizeof(packet_t));
    packet_t *p = &q->packets[pos]; memcpy(p->data,data,len); p->len=len; p->ts=ts; p->arrival=arrival;
    ++q->count; ++q->stats.accepted; return RTCP_OK;
}
int rtcp_audio_jitter_pop(rtcp_audio_jitter_t *q, uint8_t *data, size_t capacity, uint32_t *ts, uint64_t now)
{
    if (!q || !data || !ts) return RTCP_EINVAL;
    if (!q->count) return 0;
    packet_t *p=&q->packets[0];
    if (now < p->arrival || now-p->arrival < q->delay) return 0;
    if (capacity < p->len) return RTCP_EINVAL;
    size_t len=p->len; memcpy(data,p->data,len); *ts=p->ts;
    q->played=true; q->last=p->ts;
    --q->count; memmove(q->packets,q->packets+1,q->count*sizeof(packet_t)); return (int)len;
}
void rtcp_audio_jitter_stats(const rtcp_audio_jitter_t *q, rtcp_audio_jitter_stats_t *s)
{ if(q && s) { *s=q->stats; s->queued=q->count; } }
