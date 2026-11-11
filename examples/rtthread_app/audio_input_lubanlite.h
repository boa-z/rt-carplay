/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "airplay_audio_sender.h"
bool carplay_audio_input_available(void);
bool carplay_audio_input_start(const airplay_audio_input_config_t *);
void carplay_audio_input_stop(void);
void carplay_audio_input_request_stop(void);
void carplay_audio_input_mute(bool);
void carplay_audio_input_reference(const int16_t *stereo, size_t frames);
void carplay_audio_input_status(void);
