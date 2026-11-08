/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Luban-Lite video sink: MPP H.264 decoder -> DE video layer.
 *
 * The AirPlay Screen stream delivers Annex-B access units.  This sink splits
 * them into NAL units, decodes them on its own thread and presents the
 * resulting NV12 frames through the "aicfb" device.  It is the on-board
 * counterpart of the Linux simulator's H.264 dump and requires the SDK's
 * VE driver (CONFIG_AIC_MPP_H264_DEC_ENABLE).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Starts the decoder and takes over the video layer.  Returns 0 on success,
 * a negative RTCP error otherwise.  Safe to call again after stop(). */
int carplay_video_sink_start(uint16_t display_width, uint16_t display_height);

/* Stops the thread, releases in-flight frames, disables the video layer and
 * restores the UI layer alpha. */
void carplay_video_sink_stop(void);

bool carplay_video_sink_is_running(void);

/* Drop-in carplay_app_screen_cb_t: queues the Annex-B data and returns. */
void carplay_video_sink_frame(bool configuration, const uint8_t *annex_b,
                              size_t annex_b_len, uint64_t sender_time_ns,
                              void *context);
