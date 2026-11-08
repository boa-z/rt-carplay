/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pairing routes follow DiPlay AirPlaySession.kt. RTSP framing remains in
 * airplay_rtsp_codec; session crypto and persistence are provider-driven.
 */
#include "airplay_rtsp_pairing.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define PAIRING_CONTENT_TYPE "application/pairing+tlv8"
#define PAIRING_RESPONSE_MAX_BYTES 512U

struct airplay_rtsp_pairing {
    airplay_pair_setup_t *pair_setup;
    airplay_pair_verify_t *pair_verify;
};

static airplay_rtsp_pairing_t *rtsp_pairing_calloc(void)
{
    return calloc(1U, sizeof(airplay_rtsp_pairing_t));
}

static void rtsp_pairing_free(airplay_rtsp_pairing_t *session)
{
    free(session);
}

static bool path_has_suffix(const char *path, const char *suffix)
{
    if (path == NULL || suffix == NULL) return false;
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (path_len < suffix_len) return false;
    const char *candidate = path + path_len - suffix_len;
    for (size_t i = 0; i < suffix_len; ++i) {
        char left = candidate[i];
        char right = suffix[i];
        if (left >= 'A' && left <= 'Z') left = (char)(left + ('a' - 'A'));
        if (right >= 'A' && right <= 'Z') right = (char)(right + ('a' - 'A'));
        if (left != right) return false;
    }
    return true;
}

static airplay_rtsp_pairing_result_t build_response(
    const airplay_rtsp_request_t *request,
    unsigned status,
    const char *reason,
    const char *content_type,
    const uint8_t *body,
    size_t body_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    if (airplay_rtsp_build_response(request, status, reason, content_type,
                                    body, body_len, response,
                                    response_capacity, response_len) == 0) {
        return AIRPLAY_RTSP_PAIRING_HANDLED;
    }
    return response_capacity < body_len + 64U
               ? AIRPLAY_RTSP_PAIRING_RESPONSE_TOO_LARGE
               : AIRPLAY_RTSP_PAIRING_INTERNAL_ERROR;
}

airplay_rtsp_pairing_result_t airplay_rtsp_pairing_create(
    const airplay_rtsp_pairing_config_t *config,
    airplay_rtsp_pairing_t **out_session)
{
    if (out_session == NULL) return AIRPLAY_RTSP_PAIRING_INVALID_ARGUMENT;
    *out_session = NULL;
    if (config == NULL) return AIRPLAY_RTSP_PAIRING_INVALID_ARGUMENT;
    airplay_rtsp_pairing_t *session = rtsp_pairing_calloc();
    if (session == NULL) return AIRPLAY_RTSP_PAIRING_INTERNAL_ERROR;
    airplay_pair_setup_result_t setup_result = airplay_pair_setup_create(
        &config->pair_setup, &session->pair_setup);
    if (setup_result != AIRPLAY_PAIR_SETUP_OK) {
        rtsp_pairing_free(session);
        return AIRPLAY_RTSP_PAIRING_INVALID_ARGUMENT;
    }
    airplay_pair_verify_result_t verify_result = airplay_pair_verify_create(
        &config->pair_verify, &session->pair_verify);
    if (verify_result != AIRPLAY_PAIR_VERIFY_OK) {
        airplay_pair_setup_destroy(session->pair_setup);
        rtsp_pairing_free(session);
        return verify_result == AIRPLAY_PAIR_VERIFY_NO_MEMORY
                   ? AIRPLAY_RTSP_PAIRING_INTERNAL_ERROR
                   : AIRPLAY_RTSP_PAIRING_INVALID_ARGUMENT;
    }
    *out_session = session;
    return AIRPLAY_RTSP_PAIRING_HANDLED;
}

airplay_rtsp_pairing_result_t airplay_rtsp_pairing_route(
    airplay_rtsp_pairing_t *session,
    const airplay_rtsp_request_t *request,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len,
    airplay_pair_setup_result_t *pair_setup_result,
    airplay_pair_verify_result_t *pair_verify_result)
{
    if (response_len != NULL) *response_len = 0U;
    if (pair_setup_result != NULL) *pair_setup_result = AIRPLAY_PAIR_SETUP_OK;
    if (pair_verify_result != NULL) *pair_verify_result = AIRPLAY_PAIR_VERIFY_OK;
    if (session == NULL || request == NULL || response == NULL ||
        response_len == NULL) {
        return AIRPLAY_RTSP_PAIRING_INVALID_ARGUMENT;
    }
    bool is_setup = path_has_suffix(request->path, "/pair-setup");
    bool is_verify = path_has_suffix(request->path, "/pair-verify");
    if (!is_setup && !is_verify) return AIRPLAY_RTSP_PAIRING_NOT_HANDLED;
    if (strcmp(request->method, "POST") != 0) {
        return build_response(request, 405U, "Method Not Allowed", NULL,
                              NULL, 0U, response, response_capacity,
                              response_len);
    }
    if (request->body == NULL || request->body_len == 0U ||
        request->body_len > AIRPLAY_PAIR_SETUP_MAX_MESSAGE_BYTES) {
        unsigned status = request->body_len > AIRPLAY_PAIR_SETUP_MAX_MESSAGE_BYTES
                              ? 413U : 400U;
        const char *reason = status == 413U ? "Request Entity Too Large"
                                             : "Bad Request";
        return build_response(request, status, reason, NULL,
                              NULL, 0U, response, response_capacity,
                              response_len);
    }

    uint8_t body[PAIRING_RESPONSE_MAX_BYTES];
    size_t body_len = 0U;
    if (is_setup) {
        airplay_pair_setup_result_t result = airplay_pair_setup_handle(
            session->pair_setup, request->body, request->body_len, body,
            sizeof(body), &body_len);
        if (pair_setup_result != NULL) *pair_setup_result = result;
        if (result != AIRPLAY_PAIR_SETUP_OK && body_len == 0U) {
            return AIRPLAY_RTSP_PAIRING_INTERNAL_ERROR;
        }
    } else {
        airplay_pair_verify_result_t result = airplay_pair_verify_handle(
            session->pair_verify, request->body, request->body_len, body,
            sizeof(body), &body_len);
        if (pair_verify_result != NULL) *pair_verify_result = result;
        if (result != AIRPLAY_PAIR_VERIFY_OK && body_len == 0U) {
            return AIRPLAY_RTSP_PAIRING_INTERNAL_ERROR;
        }
    }
    airplay_rtsp_pairing_result_t result = build_response(
        request, 200U, "OK", PAIRING_CONTENT_TYPE, body, body_len,
        response, response_capacity, response_len);
    memset(body, 0, sizeof(body));
    return result;
}

airplay_pair_verify_result_t airplay_rtsp_pairing_get_control_keys(
    const airplay_rtsp_pairing_t *session,
    airplay_pair_verify_control_keys_t *out_keys)
{
    if (session == NULL) return AIRPLAY_PAIR_VERIFY_INVALID_ARGUMENT;
    return airplay_pair_verify_get_control_keys(session->pair_verify, out_keys);
}

airplay_pair_verify_result_t airplay_rtsp_pairing_get_event_keys(
    const airplay_rtsp_pairing_t *session,
    airplay_pair_verify_event_keys_t *out_keys)
{
    if (session == NULL) return AIRPLAY_PAIR_VERIFY_INVALID_ARGUMENT;
    return airplay_pair_verify_get_event_keys(session->pair_verify, out_keys);
}

airplay_pair_verify_result_t airplay_rtsp_pairing_derive_screen_output_key(
    const airplay_rtsp_pairing_t *session,
    uint64_t stream_connection_id,
    uint8_t output_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES])
{
    if (session == NULL) return AIRPLAY_PAIR_VERIFY_INVALID_ARGUMENT;
    return airplay_pair_verify_derive_screen_output_key(
        session->pair_verify, stream_connection_id, output_key);
}

void airplay_rtsp_pairing_destroy(airplay_rtsp_pairing_t *session)
{
    if (session == NULL) return;
    airplay_pair_setup_destroy(session->pair_setup);
    airplay_pair_verify_destroy(session->pair_verify);
    memset(session, 0, sizeof(*session));
    rtsp_pairing_free(session);
}
