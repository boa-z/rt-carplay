/* SPDX-License-Identifier: GPL-3.0-or-later; command shape follows DiPlay */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Encode the bounded main-screen forceKeyFrame command used for video recovery. */
bool airplay_command_plist_encode_force_keyframe(uint8_t *output,
                                                 size_t output_capacity,
                                                 size_t *output_len);
