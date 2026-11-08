/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "airplay_pair_setup.h"
#include "airplay_pair_verify.h"
#include "airplay_rtsp_codec.h"

typedef struct airplay_rtsp_pairing airplay_rtsp_pairing_t;

typedef enum {
    AIRPLAY_RTSP_PAIRING_HANDLED = 0,
    AIRPLAY_RTSP_PAIRING_NOT_HANDLED,
    AIRPLAY_RTSP_PAIRING_INVALID_ARGUMENT,
    AIRPLAY_RTSP_PAIRING_RESPONSE_TOO_LARGE,
    AIRPLAY_RTSP_PAIRING_INTERNAL_ERROR,
} airplay_rtsp_pairing_result_t;

typedef struct {
    airplay_pair_setup_config_t pair_setup;
    airplay_pair_verify_config_t pair_verify;
} airplay_rtsp_pairing_config_t;

airplay_rtsp_pairing_result_t airplay_rtsp_pairing_create(
    const airplay_rtsp_pairing_config_t *config,
    airplay_rtsp_pairing_t **out_session);

/* Routes POST requests ending in /pair-setup or /pair-verify. Pairing-level
 * authentication failures are returned as status-200 TLV8 error bodies, as
 * required by the protocol. Other RTSP methods/paths are left to the caller. */
airplay_rtsp_pairing_result_t airplay_rtsp_pairing_route(
    airplay_rtsp_pairing_t *session,
    const airplay_rtsp_request_t *request,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len,
    airplay_pair_setup_result_t *pair_setup_result,
    airplay_pair_verify_result_t *pair_verify_result);

airplay_pair_verify_result_t airplay_rtsp_pairing_get_control_keys(
    const airplay_rtsp_pairing_t *session,
    airplay_pair_verify_control_keys_t *out_keys);

airplay_pair_verify_result_t airplay_rtsp_pairing_get_event_keys(
    const airplay_rtsp_pairing_t *session,
    airplay_pair_verify_event_keys_t *out_keys);

airplay_pair_verify_result_t airplay_rtsp_pairing_derive_screen_output_key(
    const airplay_rtsp_pairing_t *session,
    uint64_t stream_connection_id,
    uint8_t output_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES]);

void airplay_rtsp_pairing_destroy(airplay_rtsp_pairing_t *session);
