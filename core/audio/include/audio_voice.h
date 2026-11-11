/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define RTCP_VOICE_FRAME 480U /* 10 ms mono at 48 kHz */
typedef struct rtcp_audio_voice rtcp_audio_voice_t;
/* SpeexDSP fixed-point NS/AEC plus a bounded RMS AGC. AEC requires a far-end
 * frame aligned to the acoustic path; NULL far-end bypasses cancellation.
 * One capture-worker owner. Never claim calibrated echo performance from
 * enabling this primitive alone. */
rtcp_audio_voice_t *rtcp_audio_voice_create(bool ns, bool agc, bool aec);
int rtcp_audio_voice_process(rtcp_audio_voice_t *, const int16_t near_end[RTCP_VOICE_FRAME],
                             const int16_t *far_end, int16_t out[RTCP_VOICE_FRAME]);
void rtcp_audio_voice_destroy(rtcp_audio_voice_t *);
