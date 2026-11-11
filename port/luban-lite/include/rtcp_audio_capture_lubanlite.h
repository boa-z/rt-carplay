/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
/* SDK-specific exclusive dmic0 capture. Fixed 48 kHz stereo protects the
 * shared playback/capture sampling clock. All APIs have one worker owner. */
typedef struct rtcp_audio_capture rtcp_audio_capture_t;
int rtcp_audio_capture_open(rtcp_audio_capture_t **);
/* Nonblocking read of host-endian stereo frames. Returns frames or <0. */
int rtcp_audio_capture_read(rtcp_audio_capture_t *, int16_t *pcm, size_t max_frames);
void rtcp_audio_capture_close(rtcp_audio_capture_t *);
