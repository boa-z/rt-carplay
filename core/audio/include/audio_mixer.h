/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RTCP_AUDIO_LANES 9U
#define RTCP_AUDIO_LOCAL_LANE 8U
#define RTCP_AUDIO_MIX_RATE 48000U
/* Lanes 0..7 match AirPlay semantic slots; lane 8 is a protected local alert.
 * Single audio-worker ownership. Output is interleaved stereo S16. */
typedef struct rtcp_audio_mixer rtcp_audio_mixer_t;
typedef struct {
    uint64_t accepted_frames, dropped_frames, underrun_frames, clipped_samples;
    size_t queued_frames;
    int trim_ppm;
} rtcp_audio_lane_stats_t;
rtcp_audio_mixer_t *rtcp_audio_mixer_create(size_t frames_per_lane);
void rtcp_audio_mixer_destroy(rtcp_audio_mixer_t *);
int rtcp_audio_mixer_push(rtcp_audio_mixer_t *, unsigned lane, const int16_t *,
                        size_t frames, uint32_t rate, uint8_t channels);
/* Optional bounded occupancy servo: target queue depth in source frames.
 * Call after startup buffering. Zero disables; correction is capped at 300 ppm. */
void rtcp_audio_mixer_clock(rtcp_audio_mixer_t *, unsigned lane, size_t target_frames);
void rtcp_audio_mixer_flush(rtcp_audio_mixer_t *, unsigned lane);
/* Independent gains, linear amplitude 0..1 Q15. Ramp length is output frames. */
void rtcp_audio_mixer_gain(rtcp_audio_mixer_t *, unsigned lane, unsigned q15, unsigned ramp_frames);
void rtcp_audio_mixer_auto_duck(rtcp_audio_mixer_t *, bool enabled);
void rtcp_audio_mixer_duck(rtcp_audio_mixer_t *, bool ducked);
/* A safety alert cannot be muted by entertainment volume or remote ducking. */
void rtcp_audio_mixer_master(rtcp_audio_mixer_t *, unsigned q15, bool mute);
/* Longest queued lane duration in nominal output frames (rounded down). */
size_t rtcp_audio_mixer_buffered(const rtcp_audio_mixer_t *);
size_t rtcp_audio_mixer_render(rtcp_audio_mixer_t *, int16_t *stereo, size_t frames);
void rtcp_audio_mixer_stats(const rtcp_audio_mixer_t *, unsigned lane, rtcp_audio_lane_stats_t *);
