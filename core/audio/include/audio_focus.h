/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool call_active, siri_active, navigation_active, local_active, critical_active;
    uint32_t remote_audio_entity; /* observed state, not a fabricated ownership grant */
} rtcp_audio_focus_t;
typedef struct { unsigned main_gain, alternate_gain, media_gain, local_gain; } rtcp_audio_focus_policy_t;
void rtcp_audio_focus_policy(const rtcp_audio_focus_t *, rtcp_audio_focus_policy_t *);
