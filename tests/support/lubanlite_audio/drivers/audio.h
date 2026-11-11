/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#define AUDIO_TYPE_OUTPUT 1
#define AUDIO_DSP_PARAM 2
#define AUDIO_CTL_CONFIGURE 3
#define AUDIO_STREAM_REPLAY 4
#define AUDIO_CTL_START 5
#define AUDIO_CTL_STOP 6
#define AUDIO_TYPE_MIXER 7
#define AUDIO_MIXER_VOLUME 8
struct rt_audio_caps { int main_type, sub_type; union { struct { unsigned samplerate,channels,samplebits; } config; int value; } udata; };
