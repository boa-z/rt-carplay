/* SPDX-License-Identifier: GPL-3.0-or-later; RTSP event framing follows DiPlay */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airplay_rtsp_codec.h"

#define AIRPLAY_CONTROL_STREAM_MAX_MESSAGE_BYTES 4096U
#define AIRPLAY_CONTROL_STREAM_MAX_BODY_BYTES 2048U

typedef struct airplay_control_stream airplay_control_stream_t;

typedef enum {
    AIRPLAY_CONTROL_STREAM_OK = 0,
    AIRPLAY_CONTROL_STREAM_INCOMPLETE,
    AIRPLAY_CONTROL_STREAM_INVALID_ARGUMENT,
    AIRPLAY_CONTROL_STREAM_NO_MEMORY,
    AIRPLAY_CONTROL_STREAM_TOO_LARGE,
    AIRPLAY_CONTROL_STREAM_INVALID_MESSAGE,
    AIRPLAY_CONTROL_STREAM_CALLBACK_FAILED,
} airplay_control_stream_result_t;

typedef bool (*airplay_control_stream_request_cb_t)(
    const airplay_rtsp_request_t *request,
    void *context);

airplay_control_stream_result_t airplay_control_stream_create(
    airplay_control_stream_t **out_stream);

/* Buffers partial plaintext and dispatches each complete RTSP request once.
 * RTSP/HTTP responses are consumed without dispatch. The request view is valid
 * only until the callback returns. */
airplay_control_stream_result_t airplay_control_stream_feed(
    airplay_control_stream_t *stream,
    const uint8_t *plaintext,
    size_t plaintext_len,
    airplay_control_stream_request_cb_t on_request,
    void *context);

void airplay_control_stream_destroy(airplay_control_stream_t *stream);
