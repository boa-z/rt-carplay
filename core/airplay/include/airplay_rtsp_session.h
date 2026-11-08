/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airplay_rtsp_pairing.h"

#define AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES 3500U
#define AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES 4096U

typedef struct airplay_rtsp_session airplay_rtsp_session_t;

typedef bool (*airplay_rtsp_start_screen_cb_t)(
    const airplay_rtsp_pairing_t *pairing,
    uint64_t stream_connection_id,
    uint16_t requested_port,
    uint16_t *bound_port,
    void *context);
typedef bool (*airplay_rtsp_start_event_cb_t)(
    const airplay_rtsp_pairing_t *pairing,
    uint16_t *bound_port,
    void *context);
typedef bool (*airplay_rtsp_start_timing_cb_t)(
    uint16_t peer_port,
    uint16_t *bound_port,
    void *context);
typedef void (*airplay_rtsp_stop_media_cb_t)(void *context);

typedef struct {
    airplay_rtsp_pairing_config_t pairing;
    const uint8_t *info_plist;
    size_t info_plist_len;
    uint16_t screen_requested_port;
    airplay_rtsp_start_screen_cb_t start_screen;
    airplay_rtsp_start_event_cb_t start_event;
    airplay_rtsp_start_timing_cb_t start_timing;
    airplay_rtsp_stop_media_cb_t stop_screen;
    airplay_rtsp_stop_media_cb_t stop_event;
    airplay_rtsp_stop_media_cb_t stop_timing;
    void *media_context;
} airplay_rtsp_session_config_t;

typedef enum {
    AIRPLAY_RTSP_SESSION_OK = 0,
    AIRPLAY_RTSP_SESSION_INVALID_ARGUMENT,
    AIRPLAY_RTSP_SESSION_NO_MEMORY,
    AIRPLAY_RTSP_SESSION_INTERNAL_ERROR,
} airplay_rtsp_session_result_t;

airplay_rtsp_session_result_t airplay_rtsp_session_create(
    const airplay_rtsp_session_config_t *config,
    airplay_rtsp_session_t **out_session);

/* Creates a session that borrows config->info_plist. The bytes must remain
 * immutable and valid until airplay_rtsp_session_destroy(). */
airplay_rtsp_session_result_t airplay_rtsp_session_create_borrowing_info_plist(
    const airplay_rtsp_session_config_t *config,
    airplay_rtsp_session_t **out_session);

/* Handles one parsed RTSP request and writes one complete response. The caller
 * must send the response before switching the RTSP socket to ControlCipher. */
airplay_rtsp_session_result_t airplay_rtsp_session_handle(
    airplay_rtsp_session_t *session,
    const airplay_rtsp_request_t *request,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len,
    bool *enable_control_cipher);

airplay_pair_verify_result_t airplay_rtsp_session_get_control_keys(
    const airplay_rtsp_session_t *session,
    airplay_pair_verify_control_keys_t *out_keys);
void airplay_rtsp_session_mark_control_cipher_active(
    airplay_rtsp_session_t *session);
void airplay_rtsp_session_destroy(airplay_rtsp_session_t *session);
