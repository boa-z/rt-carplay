/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "include/audio_mixer.h"
#include "rtcp/rtcp.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {
    int16_t *pcm;
    size_t head, count;
    uint32_t rate;
    uint8_t channels;
    uint64_t phase;
    size_t target_fill;
    int64_t fraction;
    int32_t gain, start, target;
    unsigned ramp, elapsed;
    rtcp_audio_lane_stats_t stats;
} lane_t;
struct rtcp_audio_mixer {
    lane_t lanes[RTCP_AUDIO_LANES];
    size_t capacity;
    unsigned master;
    bool mute, duck, auto_duck;
};
rtcp_audio_mixer_t *rtcp_audio_mixer_create(size_t capacity)
{
    if (capacity < 2 || capacity > 48000) return NULL;
    rtcp_audio_mixer_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->capacity = capacity; m->master = 32768; m->auto_duck = true;
    for (unsigned i = 0; i < RTCP_AUDIO_LANES; ++i) {
        m->lanes[i].pcm = calloc(capacity * 2, sizeof(int16_t));
        m->lanes[i].gain = m->lanes[i].target = 32768;
        if (!m->lanes[i].pcm) { rtcp_audio_mixer_destroy(m); return NULL; }
    }
    return m;
}
void rtcp_audio_mixer_destroy(rtcp_audio_mixer_t *m)
{
    if (!m) return;
    for (unsigned i = 0; i < RTCP_AUDIO_LANES; ++i) free(m->lanes[i].pcm);
    free(m);
}
void rtcp_audio_mixer_flush(rtcp_audio_mixer_t *m, unsigned i)
{
    if (!m || i >= RTCP_AUDIO_LANES) return;
    lane_t *l = &m->lanes[i]; l->count = l->head = 0; l->phase = 0; l->fraction = 0; l->stats.trim_ppm = 0;
}
int rtcp_audio_mixer_push(rtcp_audio_mixer_t *m, unsigned i, const int16_t *pcm,
                        size_t frames, uint32_t rate, uint8_t channels)
{
    if (!m || i >= RTCP_AUDIO_LANES || !pcm || !frames || rate < 8000 || rate > 48000 ||
        (channels != 1 && channels != 2)) return RTCP_EINVAL;
    lane_t *l = &m->lanes[i];
    if (l->rate != rate || l->channels != channels) {
        rtcp_audio_mixer_flush(m, i); l->rate = rate; l->channels = channels;
    }
    if (frames > m->capacity - l->count) { l->stats.dropped_frames += frames; return RTCP_ENOMEM; }
    for (size_t j = 0; j < frames; ++j) {
        size_t k = (l->head + l->count + j) % m->capacity;
        l->pcm[2*k] = pcm[j*channels];
        l->pcm[2*k+1] = pcm[j*channels + channels-1];
    }
    l->count += frames; l->stats.accepted_frames += frames; return RTCP_OK;
}
void rtcp_audio_mixer_clock(rtcp_audio_mixer_t *m, unsigned i, size_t target)
{
    if (!m || i >= RTCP_AUDIO_LANES) return;
    m->lanes[i].target_fill = target > m->capacity ? m->capacity : target;
}
void rtcp_audio_mixer_gain(rtcp_audio_mixer_t *m, unsigned i, unsigned gain, unsigned ramp)
{
    if (!m || i >= RTCP_AUDIO_LANES) return;
    lane_t *l = &m->lanes[i];
    l->start = l->gain; l->target = (int32_t)(gain > 32768 ? 32768 : gain);
    l->ramp = ramp; l->elapsed = 0;
    if (!ramp) l->gain = l->target;
}
void rtcp_audio_mixer_auto_duck(rtcp_audio_mixer_t *m, bool enabled) { if(m)m->auto_duck=enabled; }
void rtcp_audio_mixer_duck(rtcp_audio_mixer_t *m, bool duck) { if (m) m->duck = duck; }
void rtcp_audio_mixer_master(rtcp_audio_mixer_t *m, unsigned gain, bool mute)
{ if (m) { m->master = gain > 32768 ? 32768 : gain; m->mute = mute; } }
size_t rtcp_audio_mixer_buffered(const rtcp_audio_mixer_t *m)
{
    if (!m) return 0;
    size_t longest = 0;
    for (unsigned i = 0; i < RTCP_AUDIO_LANES; ++i) {
        const lane_t *l = &m->lanes[i];
        uint64_t duration = (uint64_t)l->count * RTCP_AUDIO_MIX_RATE;
        if (!l->rate || duration <= l->phase) continue;
        size_t frames = (size_t)((duration - l->phase) / l->rate);
        if (frames > longest) longest = frames;
    }
    return longest;
}
size_t rtcp_audio_mixer_render(rtcp_audio_mixer_t *m, int16_t *out, size_t frames)
{
    if (!m || !out || frames > SIZE_MAX/2) return 0;
    bool any = false;
    for (unsigned i = 0; i < RTCP_AUDIO_LANES; ++i) any |= m->lanes[i].count != 0;
    if (!any) return 0;
    for (unsigned i = 0; i < RTCP_AUDIO_LANES; ++i) {
        lane_t *l = &m->lanes[i];
        int desired = 0;
        if (l->target_fill && l->rate) {
            int64_t error = (int64_t)l->count - (int64_t)l->target_fill;
            desired = (int)(error * 1000000 / l->rate / 10);
            if (desired > 300) desired = 300;
            if (desired < -300) desired = -300;
        }
        if (l->stats.trim_ppm < desired) ++l->stats.trim_ppm;
        if (l->stats.trim_ppm > desired) --l->stats.trim_ppm;
    }
    bool prompt = m->lanes[1].count != 0;
    bool safety = m->lanes[RTCP_AUDIO_LOCAL_LANE].count != 0;
    for (size_t f = 0; f < frames; ++f) {
        int64_t mixed[2] = {0,0};
        for (unsigned i = 0; i < RTCP_AUDIO_LANES; ++i) {
            lane_t *l = &m->lanes[i];
            if (l->elapsed < l->ramp) {
                ++l->elapsed;
                l->gain = l->start + (int32_t)(((int64_t)l->target-l->start)*l->elapsed/l->ramp);
            }
            if (!l->count) { ++l->stats.underrun_frames; l->phase = 0; continue; }
            unsigned gain = (unsigned)l->gain;
            if (i != RTCP_AUDIO_LOCAL_LANE) {
                gain = m->mute ? 0 : (unsigned)((uint64_t)gain*m->master/32768);
                if (safety || ((i == 2 || i == 7) && ((prompt && m->auto_duck) || m->duck))) gain /= 4;
            }
            size_t next = l->count > 1 ? (l->head+1)%m->capacity : l->head;
            for (unsigned c = 0; c < 2; ++c) {
                int32_t a = l->pcm[2*l->head+c], b = l->pcm[2*next+c];
                int32_t sample = a + (int32_t)(((int64_t)b-a)*(int64_t)l->phase/RTCP_AUDIO_MIX_RATE);
                mixed[c] += (int64_t)sample*gain/32768;
            }
            l->fraction += (int64_t)l->rate * l->stats.trim_ppm;
            int64_t correction = l->fraction / 1000000;
            l->fraction -= correction * 1000000;
            l->phase += (uint64_t)((int64_t)l->rate + correction);
            while (l->phase >= RTCP_AUDIO_MIX_RATE && l->count) {
                l->phase -= RTCP_AUDIO_MIX_RATE;
                l->head = (l->head+1)%m->capacity; --l->count;
            }
            if (!l->count) l->phase = 0;
        }
        for (unsigned c = 0; c < 2; ++c) {
            if (mixed[c] > INT16_MAX || mixed[c] < INT16_MIN) ++m->lanes[0].stats.clipped_samples;
            out[2*f+c] = (int16_t)(mixed[c] > INT16_MAX ? INT16_MAX : mixed[c] < INT16_MIN ? INT16_MIN : mixed[c]);
        }
    }
    return frames;
}
void rtcp_audio_mixer_stats(const rtcp_audio_mixer_t *m, unsigned i, rtcp_audio_lane_stats_t *out)
{ if (m && out && i < RTCP_AUDIO_LANES) { *out = m->lanes[i].stats; out->queued_frames = m->lanes[i].count; } }
