/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { AUDIO_TEST_TONE, AUDIO_TEST_SILENCE, AUDIO_TEST_AAC,
               AUDIO_TEST_MIC_LEVEL, AUDIO_TEST_MIC_LOOP } audio_test_mode_t;
typedef struct {
    audio_test_mode_t mode;
    unsigned rate, channel; /* stereo output: 0 both, 1 left, 2 right */
} audio_test_config_t;
typedef struct {
    bool running, muted;
    unsigned volume, frames, bytes, peak_left, peak_right, writes;
    int error;
} audio_test_status_t;
/* Caller must first stop the receiver. Device refcounts are also checked.
 * No LVGL calls or device I/O on the UI thread. Each run is bounded to 10 s. */
int carplay_audio_test_start(const audio_test_config_t *);
int carplay_audio_test_stop(void);
void carplay_audio_test_gain(unsigned percent, bool mute);
void carplay_audio_test_status(audio_test_status_t *);
