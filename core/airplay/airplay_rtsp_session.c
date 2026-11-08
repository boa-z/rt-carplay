/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RTSP session behavior follows DiPlay AirPlaySession.kt. Platform media,
 * event-socket, identity, and capability providers remain injected interfaces.
 */
#include "airplay_rtsp_session.h"

#include <stdlib.h>
#include <string.h>

#include "airplay_bplist.h"
#include "airplay_mfi_sap.h"
#include "rtcp/log.h"
#include "mbedtls/platform_util.h"

#define AIRPLAY_PLIST_CONTENT_TYPE "application/x-apple-binary-plist"
#define AIRPLAY_OCTET_CONTENT_TYPE "application/octet-stream"
#define AIRPLAY_RTSP_SESSION_SETUP_BODY_BYTES 256U
#define AIRPLAY_RTSP_SESSION_MAX_SCREEN_KEY_IDS 8U
#define AIRPLAY_RTSP_SESSION_MAX_SETUP_STREAMS 8U
#define AIRPLAY_RTSP_STATUS_OK 200U
#define AIRPLAY_RTSP_STATUS_BAD_REQUEST 400U
#define AIRPLAY_RTSP_STATUS_METHOD_NOT_ALLOWED 405U
#define AIRPLAY_RTSP_STATUS_SESSION_NOT_READY 503U

#define TAG "airplay_rtsp_session"

struct airplay_rtsp_session {
    airplay_rtsp_session_config_t config;
    airplay_rtsp_pairing_t *pairing;
    uint8_t *owned_info_plist;
    uint16_t event_port;
    uint16_t timing_port;
    uint16_t screen_port;
    uint64_t screen_connection_ids[AIRPLAY_RTSP_SESSION_MAX_SCREEN_KEY_IDS];
    size_t screen_connection_id_count;
    uint64_t active_screen_connection_id;
    bool has_info;
    bool event_started;
    bool timing_started;
    bool initial_setup_complete;
    bool screen_started;
    bool record_active;
    bool control_cipher_active;
    bool state_in_psram;
};

static airplay_rtsp_session_t *session_calloc(bool *allocated_in_psram)
{
    if (allocated_in_psram != NULL) *allocated_in_psram = false;
    return calloc(1U, sizeof(airplay_rtsp_session_t));
}

static void session_free(airplay_rtsp_session_t *session)
{
    if (session == NULL) return;
    mbedtls_platform_zeroize(session, sizeof(*session));
    free(session);
}

static bool path_has_suffix(const char *path, const char *suffix)
{
    if (path == NULL || suffix == NULL) return false;
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (path_len < suffix_len) return false;
    const char *candidate = path + path_len - suffix_len;
    for (size_t i = 0U; i < suffix_len; ++i) {
        char left = candidate[i];
        char right = suffix[i];
        if (left >= 'A' && left <= 'Z') left = (char)(left + ('a' - 'A'));
        if (right >= 'A' && right <= 'Z') right = (char)(right + ('a' - 'A'));
        if (left != right) return false;
    }
    return true;
}

static airplay_rtsp_session_result_t build_response(
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
        return AIRPLAY_RTSP_SESSION_OK;
    }
    return AIRPLAY_RTSP_SESSION_INTERNAL_ERROR;
}

static airplay_rtsp_session_result_t status_response(
    const airplay_rtsp_request_t *request,
    unsigned status,
    const char *reason,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    return build_response(request, status, reason, NULL, NULL, 0U,
                          response, response_capacity, response_len);
}

static airplay_rtsp_session_result_t screen_setup_response(
    const airplay_rtsp_request_t *request,
    uint64_t stream_type,
    uint16_t data_port,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    uint8_t body[AIRPLAY_RTSP_SESSION_SETUP_BODY_BYTES];
    size_t body_len = 0U;
    if (airplay_bplist_build_screen_setup_response(
            stream_type, data_port, body, sizeof(body), &body_len) != AIRPLAY_BPLIST_OK) {
        return status_response(request, AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                               "Service Unavailable", response,
                               response_capacity, response_len);
    }
    airplay_rtsp_session_result_t result = build_response(
        request, AIRPLAY_RTSP_STATUS_OK, "OK", AIRPLAY_PLIST_CONTENT_TYPE,
        body, body_len, response, response_capacity, response_len);
    memset(body, 0, sizeof(body));
    return result;
}

static airplay_rtsp_session_result_t handle_mfi_auth_setup(
    const airplay_rtsp_request_t *request,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    if (strcmp(request->method, "POST") != 0) {
        return status_response(request, AIRPLAY_RTSP_STATUS_METHOD_NOT_ALLOWED,
                               "Method Not Allowed", response,
                               response_capacity, response_len);
    }

    uint8_t body[AIRPLAY_MFI_SAP_MAX_RESPONSE_BYTES];
    size_t body_len = 0U;
    airplay_mfi_sap_result_t result = airplay_mfi_sap_respond(
        request->body, request->body_len, body, sizeof(body), &body_len);
    if (result != AIRPLAY_MFI_SAP_OK) {
        mbedtls_platform_zeroize(body, sizeof(body));
        const unsigned status = result == AIRPLAY_MFI_SAP_INVALID_ARGUMENT
                                    ? AIRPLAY_RTSP_STATUS_BAD_REQUEST
                                : result == AIRPLAY_MFI_SAP_UNAVAILABLE ||
                                          result == AIRPLAY_MFI_SAP_UNSUPPORTED_PROTOCOL
                                    ? AIRPLAY_RTSP_STATUS_SESSION_NOT_READY
                                    : 500U;
        const char *reason = status == AIRPLAY_RTSP_STATUS_BAD_REQUEST
                                 ? "Bad Request"
                             : status == AIRPLAY_RTSP_STATUS_SESSION_NOT_READY
                                 ? "Service Unavailable"
                                 : "Internal Server Error";
        return status_response(request, status, reason, response,
                               response_capacity, response_len);
    }

    airplay_rtsp_session_result_t response_result = build_response(
        request, AIRPLAY_RTSP_STATUS_OK, "OK", AIRPLAY_OCTET_CONTENT_TYPE,
        body, body_len, response, response_capacity, response_len);
    mbedtls_platform_zeroize(body, sizeof(body));
    return response_result;
}

static bool screen_connection_id_was_used(const airplay_rtsp_session_t *session,
                                          uint64_t connection_id)
{
    for (size_t index = 0U; index < session->screen_connection_id_count; ++index) {
        if (session->screen_connection_ids[index] == connection_id) return true;
    }
    return false;
}

static bool teardown_stream_selection(const airplay_rtsp_request_t *request,
                                      bool *has_stream_selection,
                                      bool *screen_selected)
{
    if (has_stream_selection == NULL || screen_selected == NULL) return false;
    *has_stream_selection = false;
    *screen_selected = false;
    if (request->body == NULL || request->body_len == 0U) return true;

    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    airplay_bplist_object_t streams;
    airplay_bplist_type_t type;
    uint64_t stream_count = 0U;
    if (airplay_bplist_open(request->body, request->body_len, &doc) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_root(&doc, &root) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_type(&doc, root, &type, NULL) != AIRPLAY_BPLIST_OK ||
        type != AIRPLAY_BPLIST_DICTIONARY) {
        return false;
    }
    airplay_bplist_result_t lookup = airplay_bplist_dict_get(
        &doc, root, "streams", &streams);
    if (lookup == AIRPLAY_BPLIST_NOT_FOUND) return true;
    if (lookup != AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_type(&doc, streams, &type, &stream_count) != AIRPLAY_BPLIST_OK ||
        type != AIRPLAY_BPLIST_ARRAY ||
        stream_count > AIRPLAY_RTSP_SESSION_MAX_SETUP_STREAMS) {
        return false;
    }

    for (uint64_t index = 0U; index < stream_count; ++index) {
        airplay_bplist_object_t entry;
        airplay_bplist_object_t type_object;
        uint64_t stream_type = 0U;
        if (airplay_bplist_array_get(&doc, streams, index, &entry) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_type(&doc, entry, &type, NULL) != AIRPLAY_BPLIST_OK) {
            return false;
        }
        if (type != AIRPLAY_BPLIST_DICTIONARY ||
            airplay_bplist_dict_get(&doc, entry, "type", &type_object) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_uint(&doc, type_object, &stream_type) != AIRPLAY_BPLIST_OK) {
            continue;
        }
        *has_stream_selection = true;
        if (stream_type == 110U) *screen_selected = true;
    }
    return true;
}

static airplay_rtsp_session_result_t handle_teardown(
    airplay_rtsp_session_t *session,
    const airplay_rtsp_request_t *request,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    bool has_stream_selection = false;
    bool screen_selected = false;
    if (!teardown_stream_selection(request, &has_stream_selection,
                                   &screen_selected)) {
        return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                               "Bad Request", response, response_capacity,
                               response_len);
    }

    if (has_stream_selection) {
        if (screen_selected && session->screen_started) {
            if (session->config.stop_screen != NULL) {
                session->config.stop_screen(session->config.media_context);
            }
            session->screen_started = false;
            session->record_active = false;
            session->screen_port = 0U;
        }
        return status_response(request, AIRPLAY_RTSP_STATUS_OK, "OK",
                               response, response_capacity, response_len);
    }

    if (session->screen_started && session->config.stop_screen != NULL) {
        session->config.stop_screen(session->config.media_context);
    }
    if (session->event_started && session->config.stop_event != NULL) {
        session->config.stop_event(session->config.media_context);
    }
    if (session->timing_started && session->config.stop_timing != NULL) {
        session->config.stop_timing(session->config.media_context);
    }
    session->screen_started = false;
    session->event_started = false;
    session->timing_started = false;
    session->initial_setup_complete = false;
    session->record_active = false;
    session->screen_port = 0U;
    session->event_port = 0U;
    session->timing_port = 0U;
    return status_response(request, AIRPLAY_RTSP_STATUS_OK, "OK",
                           response, response_capacity, response_len);
}

static airplay_rtsp_session_result_t handle_setup(
    airplay_rtsp_session_t *session,
    const airplay_rtsp_request_t *request,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    airplay_bplist_object_t streams;
    airplay_bplist_object_t screen_stream = {0};
    airplay_bplist_object_t timing_port_object;
    airplay_bplist_type_t type;
    uint64_t stream_count = 0U;
    uint8_t body[AIRPLAY_RTSP_SESSION_SETUP_BODY_BYTES];
    size_t body_len = 0U;
    airplay_rtsp_session_result_t result;

    if (request->body == NULL || request->body_len == 0U ||
        airplay_bplist_open(request->body, request->body_len, &doc) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_root(&doc, &root) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_type(&doc, root, &type, NULL) != AIRPLAY_BPLIST_OK ||
        type != AIRPLAY_BPLIST_DICTIONARY) {
        return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                               "Bad Request", response, response_capacity,
                               response_len);
    }

    airplay_bplist_result_t lookup = airplay_bplist_dict_get(
        &doc, root, "streams", &streams);
    if (lookup == AIRPLAY_BPLIST_NOT_FOUND) {
        uint64_t peer_timing_port = 0U;
        airplay_bplist_result_t timing_lookup = airplay_bplist_dict_get(
            &doc, root, "timingPort", &timing_port_object);
        if (timing_lookup == AIRPLAY_BPLIST_OK &&
            (airplay_bplist_get_uint(&doc, timing_port_object,
                                     &peer_timing_port) != AIRPLAY_BPLIST_OK ||
             peer_timing_port > UINT16_MAX)) {
            return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                                   "Bad Request", response,
                                   response_capacity, response_len);
        }
        if (timing_lookup != AIRPLAY_BPLIST_OK &&
            timing_lookup != AIRPLAY_BPLIST_NOT_FOUND) {
            return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                                   "Bad Request", response,
                                   response_capacity, response_len);
        }

        bool started_event_now = false;
        bool started_timing_now = false;
        uint16_t event_port = session->event_port;
        uint16_t timing_port = session->timing_port;
        if (session->event_started && event_port == 0U) {
            session->event_started = false;
        }
        if (session->timing_started && timing_port == 0U) {
            session->timing_started = false;
        }
        if (!session->event_started) {
            event_port = 0U;
            bool started = session->config.start_event != NULL &&
                session->config.start_event(session->pairing, &event_port,
                                            session->config.media_context);
            if (!started || event_port == 0U) {
                if (started && session->config.stop_event != NULL) {
                    session->config.stop_event(session->config.media_context);
                }
                return status_response(request, AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                                       "Service Unavailable", response,
                                       response_capacity, response_len);
            }
            session->event_started = true;
            session->event_port = event_port;
            started_event_now = true;
        }
        if (!session->timing_started) {
            timing_port = 0U;
            bool started = session->config.start_timing != NULL &&
                session->config.start_timing((uint16_t)peer_timing_port,
                                             &timing_port,
                                             session->config.media_context);
            if (!started || timing_port == 0U) {
                if (started && session->config.stop_timing != NULL) {
                    session->config.stop_timing(session->config.media_context);
                }
                if (started_event_now && session->config.stop_event != NULL) {
                    session->config.stop_event(session->config.media_context);
                    session->event_started = false;
                    session->event_port = 0U;
                }
                return status_response(request, AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                                       "Service Unavailable", response,
                                       response_capacity, response_len);
            }
            session->timing_started = true;
            session->timing_port = timing_port;
            started_timing_now = true;
        }
        if (airplay_bplist_build_initial_setup_response(
                session->timing_port, session->event_port, body,
                sizeof(body), &body_len) != AIRPLAY_BPLIST_OK) {
            if (started_timing_now && session->config.stop_timing != NULL) {
                session->config.stop_timing(session->config.media_context);
                session->timing_started = false;
                session->timing_port = 0U;
            }
            if (started_event_now && session->config.stop_event != NULL) {
                session->config.stop_event(session->config.media_context);
                session->event_started = false;
                session->event_port = 0U;
            }
            return status_response(request, AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                                   "Service Unavailable", response,
                                   response_capacity, response_len);
        }
        result = build_response(request, AIRPLAY_RTSP_STATUS_OK, "OK",
                                AIRPLAY_PLIST_CONTENT_TYPE, body, body_len,
                                response, response_capacity, response_len);
        if (result == AIRPLAY_RTSP_SESSION_OK) {
            session->initial_setup_complete = true;
            RTCP_LOGI(TAG, "setup=initial timing_port=%u event_port=%u enabled_features=iAPChannel,viewAreas",
                     (unsigned)session->timing_port, (unsigned)session->event_port);
        }
        memset(body, 0, sizeof(body));
        if (result != AIRPLAY_RTSP_SESSION_OK) {
            if (started_timing_now && session->config.stop_timing != NULL) {
                session->config.stop_timing(session->config.media_context);
                session->timing_started = false;
                session->timing_port = 0U;
            }
            if (started_event_now && session->config.stop_event != NULL) {
                session->config.stop_event(session->config.media_context);
                session->event_started = false;
                session->event_port = 0U;
            }
        }
        return result;
    }
    if (lookup != AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_type(&doc, streams, &type, &stream_count) != AIRPLAY_BPLIST_OK ||
        type != AIRPLAY_BPLIST_ARRAY ||
        stream_count > AIRPLAY_RTSP_SESSION_MAX_SETUP_STREAMS) {
        return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                               "Bad Request", response, response_capacity,
                               response_len);
    }

    bool has_screen_stream = false;
    for (uint64_t index = 0U; index < stream_count; ++index) {
        airplay_bplist_object_t entry;
        airplay_bplist_object_t type_object;
        airplay_bplist_type_t entry_type;
        uint64_t candidate_type = 0U;
        if (airplay_bplist_array_get(&doc, streams, index, &entry) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_type(&doc, entry, &entry_type, NULL) != AIRPLAY_BPLIST_OK ||
            entry_type != AIRPLAY_BPLIST_DICTIONARY ||
            airplay_bplist_dict_get(&doc, entry, "type", &type_object) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_uint(&doc, type_object, &candidate_type) != AIRPLAY_BPLIST_OK) {
            return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                                   "Bad Request", response, response_capacity,
                                   response_len);
        }
        RTCP_LOGI(TAG, "setup=stream_candidate index=%u type=%llu",
                 (unsigned)index, (unsigned long long)candidate_type);
        if (candidate_type != 110U) continue;
        if (has_screen_stream) {
            return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                                   "Bad Request", response, response_capacity,
                                   response_len);
        }
        has_screen_stream = true;
        screen_stream = entry;
    }

    if (!has_screen_stream) {
        RTCP_LOGW(TAG, "setup=streams_without_main_screen count=%u", (unsigned)stream_count);
        if (airplay_bplist_build_empty_stream_setup_response(
                body, sizeof(body), &body_len) != AIRPLAY_BPLIST_OK) {
            return status_response(request, AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                                   "Service Unavailable", response,
                                   response_capacity, response_len);
        }
        result = build_response(request, AIRPLAY_RTSP_STATUS_OK, "OK",
                                AIRPLAY_PLIST_CONTENT_TYPE, body, body_len,
                                response, response_capacity, response_len);
        memset(body, 0, sizeof(body));
        return result;
    }

    airplay_bplist_object_t type_object;
    uint64_t stream_type = 0U;
    uint64_t connection_id = 0U;
    if (airplay_bplist_dict_get(&doc, screen_stream, "type", &type_object) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_uint(&doc, type_object, &stream_type) != AIRPLAY_BPLIST_OK) {
        return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                               "Bad Request", response, response_capacity,
                               response_len);
    }
    if (airplay_bplist_dict_get(&doc, screen_stream, "streamConnectionID", &type_object) !=
            AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_uint(&doc, type_object, &connection_id) != AIRPLAY_BPLIST_OK) {
        return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                               "Bad Request", response, response_capacity,
                               response_len);
    }

    if (session->screen_started &&
        connection_id == session->active_screen_connection_id) {
        return screen_setup_response(request, stream_type, session->screen_port,
                                     response, response_capacity, response_len);
    }
    if (screen_connection_id_was_used(session, connection_id) ||
        session->screen_connection_id_count >= AIRPLAY_RTSP_SESSION_MAX_SCREEN_KEY_IDS) {
        return status_response(request, AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                               "Service Unavailable", response,
                               response_capacity, response_len);
    }

    if (session->screen_started && session->config.stop_screen != NULL) {
        session->config.stop_screen(session->config.media_context);
        session->screen_started = false;
        session->screen_port = 0U;
    }
    uint16_t bound_port = 0U;
    bool screen_started = session->config.start_screen != NULL &&
        session->config.start_screen(session->pairing, connection_id,
                                      session->config.screen_requested_port,
                                      &bound_port,
                                      session->config.media_context);
    if (screen_started) {
        session->screen_connection_ids[session->screen_connection_id_count++] =
            connection_id;
        session->active_screen_connection_id = connection_id;
    }
    if (!screen_started || bound_port == 0U) {
        RTCP_LOGE(TAG, "screen_start=failed stream_type=%llu connection_id=%llu port=%u",
                 (unsigned long long)stream_type,
                 (unsigned long long)connection_id, (unsigned)bound_port);
        if (screen_started && session->config.stop_screen != NULL) {
            session->config.stop_screen(session->config.media_context);
        }
        return status_response(request, AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                               "Service Unavailable", response,
                               response_capacity, response_len);
    }
    session->screen_started = true;
    session->screen_port = bound_port;
    RTCP_LOGI(TAG, "screen_start=ready stream_type=%llu connection_id=%llu port=%u",
             (unsigned long long)stream_type,
             (unsigned long long)connection_id, (unsigned)bound_port);
    result = screen_setup_response(request, stream_type, bound_port,
                                   response, response_capacity, response_len);
    if (result != AIRPLAY_RTSP_SESSION_OK && session->config.stop_screen != NULL) {
        session->config.stop_screen(session->config.media_context);
        session->screen_started = false;
        session->screen_port = 0U;
    }
    return result;
}

static uint8_t *copy_info_plist(const uint8_t *info_plist,
                                size_t info_plist_len,
                                bool *allocated_in_psram)
{
    if (allocated_in_psram != NULL) *allocated_in_psram = false;
    uint8_t *copy = NULL;
    copy = malloc(info_plist_len);
    if (copy != NULL) memcpy(copy, info_plist, info_plist_len);
    return copy;
}

static void free_info_plist(uint8_t *info_plist, size_t info_plist_len)
{
    if (info_plist == NULL) return;
    mbedtls_platform_zeroize(info_plist, info_plist_len);
    free(info_plist);
}

static airplay_rtsp_session_result_t session_create(
    const airplay_rtsp_session_config_t *config,
    airplay_rtsp_session_t **out_session,
    bool borrow_info_plist)
{
    if (out_session == NULL) return AIRPLAY_RTSP_SESSION_INVALID_ARGUMENT;
    *out_session = NULL;
    if (config == NULL || config->info_plist == NULL ||
        config->info_plist_len == 0U ||
        config->info_plist_len > AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES) {
        return AIRPLAY_RTSP_SESSION_INVALID_ARGUMENT;
    }
    airplay_bplist_doc_t info_doc;
    airplay_bplist_object_t info_root;
    airplay_bplist_type_t info_type;
    if (airplay_bplist_open(config->info_plist, config->info_plist_len,
                            &info_doc) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_root(&info_doc, &info_root) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_type(&info_doc, info_root, &info_type, NULL) !=
            AIRPLAY_BPLIST_OK || info_type != AIRPLAY_BPLIST_DICTIONARY) {
        return AIRPLAY_RTSP_SESSION_INVALID_ARGUMENT;
    }

    bool state_in_psram = false;
    airplay_rtsp_session_t *session = session_calloc(&state_in_psram);
    if (session == NULL) return AIRPLAY_RTSP_SESSION_NO_MEMORY;
    session->state_in_psram = state_in_psram;
    airplay_rtsp_pairing_result_t pairing_result =
        airplay_rtsp_pairing_create(&config->pairing, &session->pairing);
    if (pairing_result != AIRPLAY_RTSP_PAIRING_HANDLED) {
        session_free(session);
        return pairing_result == AIRPLAY_RTSP_PAIRING_INTERNAL_ERROR
                   ? AIRPLAY_RTSP_SESSION_NO_MEMORY
                   : AIRPLAY_RTSP_SESSION_INVALID_ARGUMENT;
    }
    session->config = *config;
    if (!borrow_info_plist) {
        bool allocated_in_psram = false;
        session->owned_info_plist = copy_info_plist(
            config->info_plist, config->info_plist_len, &allocated_in_psram);
        if (session->owned_info_plist == NULL) {
            airplay_rtsp_pairing_destroy(session->pairing);
            memset(session, 0, sizeof(*session));
            session_free(session);
            return AIRPLAY_RTSP_SESSION_NO_MEMORY;
        }
        session->config.info_plist = session->owned_info_plist;
        RTCP_LOGI(TAG, "info_plist=stored memory=%s bytes=%u",
                 allocated_in_psram ? "psram" : "internal",
                 (unsigned)config->info_plist_len);
    } else {
        RTCP_LOGI(TAG, "info_plist=borrowed bytes=%u",
                 (unsigned)config->info_plist_len);
    }
    session->has_info = true;
    *out_session = session;
    RTCP_LOGI(TAG, "session_state=allocated memory=%s bytes=%u",
             session->state_in_psram ? "psram" : "internal",
             (unsigned)sizeof(*session));
    return AIRPLAY_RTSP_SESSION_OK;
}

airplay_rtsp_session_result_t airplay_rtsp_session_create(
    const airplay_rtsp_session_config_t *config,
    airplay_rtsp_session_t **out_session)
{
    return session_create(config, out_session, false);
}

airplay_rtsp_session_result_t airplay_rtsp_session_create_borrowing_info_plist(
    const airplay_rtsp_session_config_t *config,
    airplay_rtsp_session_t **out_session)
{
    return session_create(config, out_session, true);
}

airplay_rtsp_session_result_t airplay_rtsp_session_handle(
    airplay_rtsp_session_t *session,
    const airplay_rtsp_request_t *request,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len,
    bool *enable_control_cipher)
{
    if (response_len != NULL) *response_len = 0U;
    if (enable_control_cipher != NULL) *enable_control_cipher = false;
    if (session == NULL || request == NULL || response == NULL ||
        response_len == NULL || enable_control_cipher == NULL) {
        return AIRPLAY_RTSP_SESSION_INVALID_ARGUMENT;
    }

    airplay_pair_setup_result_t setup_result;
    airplay_pair_verify_result_t verify_result;
    airplay_rtsp_pairing_result_t pairing_result = airplay_rtsp_pairing_route(
        session->pairing, request, response, response_capacity, response_len,
        &setup_result, &verify_result);
    if (pairing_result == AIRPLAY_RTSP_PAIRING_HANDLED) {
        if (!session->control_cipher_active) {
            airplay_pair_verify_control_keys_t keys;
            if (airplay_rtsp_session_get_control_keys(session, &keys) ==
                AIRPLAY_PAIR_VERIFY_OK) {
                *enable_control_cipher = true;
            }
            mbedtls_platform_zeroize(&keys, sizeof(keys));
        }
        return AIRPLAY_RTSP_SESSION_OK;
    }
    if (pairing_result != AIRPLAY_RTSP_PAIRING_NOT_HANDLED) {
        return status_response(request, 500U, "Internal Server Error",
                               response, response_capacity, response_len);
    }

    if (path_has_suffix(request->path, "/auth-setup")) {
        return handle_mfi_auth_setup(request, response, response_capacity,
                                     response_len);
    }

    if (path_has_suffix(request->path, "/info")) {
        if (!session->has_info) {
            return status_response(request, AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                                   "Service Unavailable", response,
                                   response_capacity, response_len);
        }
        return build_response(request, AIRPLAY_RTSP_STATUS_OK, "OK",
                              AIRPLAY_PLIST_CONTENT_TYPE,
                              session->config.info_plist,
                              session->config.info_plist_len,
                              response, response_capacity, response_len);
    }

    if (strcmp(request->method, "SETUP") == 0) {
        return handle_setup(session, request, response, response_capacity,
                            response_len);
    }
    if (strcmp(request->method, "RECORD") == 0) {
        /* Like DiPlay, accept session activation before the later Screen
         * SETUP. RECORD acknowledges the transport session, not video receipt. */
        if (!session->initial_setup_complete && !session->screen_started) {
            RTCP_LOGW(TAG, "record=rejected reason=session_not_setup");
            return status_response(request,
                                   AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                                   "Service Unavailable", response,
                                   response_capacity, response_len);
        }
        airplay_rtsp_session_result_t result = status_response(
            request, AIRPLAY_RTSP_STATUS_OK, "OK", response,
            response_capacity, response_len);
        if (result == AIRPLAY_RTSP_SESSION_OK) {
            session->record_active = true;
            RTCP_LOGI(TAG, "record=accepted phase=%s screen_port=%u",
                     session->screen_started ? "screen_ready" : "awaiting_screen_setup",
                     (unsigned)session->screen_port);
        }
        return result;
    }
    if (strcmp(request->method, "TEARDOWN") == 0) {
        return handle_teardown(session, request, response,
                               response_capacity, response_len);
    }
    if (strcmp(request->method, "OPTIONS") == 0 ||
        strcmp(request->method, "GET_PARAMETER") == 0 ||
        strcmp(request->method, "SET_PARAMETER") == 0) {
        return status_response(request, AIRPLAY_RTSP_STATUS_OK, "OK",
                               response, response_capacity, response_len);
    }
    return status_response(request, AIRPLAY_RTSP_STATUS_METHOD_NOT_ALLOWED,
                           "Method Not Allowed", response,
                           response_capacity, response_len);
}

airplay_pair_verify_result_t airplay_rtsp_session_get_control_keys(
    const airplay_rtsp_session_t *session,
    airplay_pair_verify_control_keys_t *out_keys)
{
    if (session == NULL) return AIRPLAY_PAIR_VERIFY_INVALID_ARGUMENT;
    return airplay_rtsp_pairing_get_control_keys(session->pairing, out_keys);
}

void airplay_rtsp_session_mark_control_cipher_active(
    airplay_rtsp_session_t *session)
{
    if (session != NULL) session->control_cipher_active = true;
}

void airplay_rtsp_session_destroy(airplay_rtsp_session_t *session)
{
    if (session == NULL) return;
    if (session->screen_started && session->config.stop_screen != NULL) {
        session->config.stop_screen(session->config.media_context);
    }
    if (session->event_started && session->config.stop_event != NULL) {
        session->config.stop_event(session->config.media_context);
    }
    if (session->timing_started && session->config.stop_timing != NULL) {
        session->config.stop_timing(session->config.media_context);
    }
    airplay_rtsp_pairing_destroy(session->pairing);
    free_info_plist(session->owned_info_plist,
                    session->config.info_plist_len);
    memset(session, 0, sizeof(*session));
    session_free(session);
}
