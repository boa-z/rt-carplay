/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RTSP session behavior follows DiPlay AirPlaySession.kt. Platform media,
 * event-socket, identity, and capability providers remain injected interfaces.
 */
#include "airplay_rtsp_session.h"

#include <stdlib.h>
#include <string.h>

#include "airplay_audio.h"
#include "airplay_bplist.h"
#include "airplay_mfi_sap.h"
#include "rtcp/log.h"
#include "mbedtls/platform_util.h"

#define AIRPLAY_PLIST_CONTENT_TYPE "application/x-apple-binary-plist"
#define AIRPLAY_OCTET_CONTENT_TYPE "application/octet-stream"
#define AIRPLAY_RTSP_SESSION_SETUP_BODY_BYTES 2048U
#define AIRPLAY_RTSP_SESSION_MAX_DATA_STREAM_KEY_IDS 64U
#define AIRPLAY_RTSP_SESSION_MAX_SETUP_STREAMS 8U
#define AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS AIRPLAY_AUDIO_MAX_STREAMS
#define AIRPLAY_RTSP_STATUS_OK 200U
#define AIRPLAY_RTSP_STATUS_BAD_REQUEST 400U
#define AIRPLAY_RTSP_STATUS_METHOD_NOT_ALLOWED 405U
#define AIRPLAY_RTSP_STATUS_SESSION_NOT_READY 503U

#define TAG "airplay_rtsp_session"

typedef struct {
    bool started;
    uint16_t stream_type;
    uint16_t data_port;
    uint16_t control_port;
    uint64_t connection_id;
    airplay_audio_options_t options;
} session_audio_stream_t;

struct airplay_rtsp_session {
    airplay_rtsp_session_config_t config;
    airplay_rtsp_pairing_t *pairing;
    uint8_t *owned_info_plist;
    uint16_t event_port;
    uint16_t timing_port;
    uint16_t screen_port;
    uint64_t data_stream_connection_ids[AIRPLAY_RTSP_SESSION_MAX_DATA_STREAM_KEY_IDS];
    size_t data_stream_connection_id_count;
    uint64_t active_screen_connection_id;
    session_audio_stream_t audio[AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS];
    bool has_info;
    bool info_logged;
    bool event_started;
    bool timing_started;
    bool initial_setup_complete;
    bool screen_started;
    bool record_active;
    bool control_cipher_active;
    bool state_in_psram;
    bool iap_started;
    uint64_t iap_seed;
    uint16_t iap_port;
    unsigned modes_changes;
    unsigned unsupported_commands;
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

static airplay_rtsp_session_result_t streams_setup_response(
    const airplay_rtsp_request_t *request,
    const airplay_bplist_stream_entry_t *entries,
    size_t entry_count,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    uint8_t body[AIRPLAY_RTSP_SESSION_SETUP_BODY_BYTES];
    size_t body_len = 0U;
    airplay_bplist_result_t built =
        entry_count == 0U
            ? airplay_bplist_build_empty_stream_setup_response(
                  body, sizeof(body), &body_len)
            : airplay_bplist_build_streams_setup_response(
                  entries, entry_count, body, sizeof(body), &body_len);
    if (built != AIRPLAY_BPLIST_OK) {
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
    RTCP_LOGI(TAG, "auth_setup result=%d request=%u response=%u",
              (int)result, (unsigned)request->body_len,
              (unsigned)body_len);
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

static bool data_stream_connection_id_was_used(
    const airplay_rtsp_session_t *session, uint64_t connection_id)
{
    for (size_t index = 0U; index < session->data_stream_connection_id_count;
         ++index) {
        if (session->data_stream_connection_ids[index] == connection_id) {
            return true;
        }
    }
    return false;
}

static bool remember_data_stream_connection_id(airplay_rtsp_session_t *session,
                                               uint64_t connection_id)
{
    if (session->data_stream_connection_id_count >=
        AIRPLAY_RTSP_SESSION_MAX_DATA_STREAM_KEY_IDS) {
        return false;
    }
    session->data_stream_connection_ids[session->data_stream_connection_id_count++] =
        connection_id;
    return true;
}

static void stop_audio_endpoint(airplay_rtsp_session_t *s,uint16_t type,const airplay_audio_options_t *options)
{
    if(s->config.stop_audio_ex)s->config.stop_audio_ex(type,options,s->config.media_context);
    else if(s->config.stop_audio)s->config.stop_audio(type,s->config.media_context);
}
static void session_stop_audio_slot(airplay_rtsp_session_t *s,unsigned index)
{
    if(index>=AIRPLAY_AUDIO_MAX_STREAMS || !s->audio[index].started)return;
    session_audio_stream_t *slot=&s->audio[index];
    stop_audio_endpoint(s,slot->stream_type,&slot->options);
    memset(slot,0,sizeof(*slot));
}
static void session_stop_all_audio(airplay_rtsp_session_t *s)
{ for(unsigned i=0;i<AIRPLAY_AUDIO_MAX_STREAMS;++i)session_stop_audio_slot(s,i); }

static bool teardown_stream_selection(const airplay_rtsp_session_t *session, const airplay_rtsp_request_t *request,
                                      bool *has_stream_selection,
                                      bool *screen_selected,
                                      uint8_t *audio_type_mask, bool *iap_selected)
{
    if (has_stream_selection == NULL || screen_selected == NULL ||
        audio_type_mask == NULL) {
        return false;
    }
    *iap_selected = false;
    *has_stream_selection = false;
    *screen_selected = false;
    *audio_type_mask = 0U;
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
        if (stream_type == 130U) {
            airplay_bplist_object_t stream_id;
            uint64_t id = 1;
            airplay_bplist_result_t got = airplay_bplist_dict_get(&doc, entry, "streamID", &stream_id);
            if (got == AIRPLAY_BPLIST_OK) {
                if (airplay_bplist_get_uint(&doc, stream_id, &id) != AIRPLAY_BPLIST_OK) return false;
            } else if (got != AIRPLAY_BPLIST_NOT_FOUND) return false;
            if (id == 1U) *iap_selected = true;
        }
        if(airplay_audio_stream_type_is_supported(stream_type)) {
            uint64_t connection=0;bool has_connection=false;
            airplay_bplist_object_t selected;
            airplay_bplist_result_t got=airplay_bplist_dict_get(&doc,entry,"streamConnectionID",&selected);
            if(got==AIRPLAY_BPLIST_OK) {
                if(airplay_bplist_get_uint(&doc,selected,&connection)!=AIRPLAY_BPLIST_OK)return false;
                has_connection=true;
            } else if(got!=AIRPLAY_BPLIST_NOT_FOUND)return false;
            char role[24]={0};size_t text=0;
            got=airplay_bplist_dict_get(&doc,entry,"audioType",&selected);
            if(got==AIRPLAY_BPLIST_OK && (airplay_bplist_get_string(&doc,selected,role,sizeof(role),&text)!=AIRPLAY_BPLIST_OK || !text || strlen(role)!=text))return false;
            for(unsigned slot=0;slot<AIRPLAY_AUDIO_MAX_STREAMS;++slot) {
                const session_audio_stream_t *audio=&session->audio[slot];
                if(audio->started && audio->stream_type==stream_type &&
                   (!has_connection || audio->connection_id==connection) &&
                   (!text || !strcmp(role,audio->options.audio_type))) *audio_type_mask|=(uint8_t)(1U<<slot);
            }
        }
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
    bool iap_selected = false;
    bool has_stream_selection = false;
    bool screen_selected = false;
    uint8_t audio_type_mask = 0U;
    if (!teardown_stream_selection(session, request, &has_stream_selection,
                                   &screen_selected, &audio_type_mask, &iap_selected)) {
        return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                               "Bad Request", response, response_capacity,
                               response_len);
    }

    if (has_stream_selection) {
        if (iap_selected && session->iap_port && session->config.stop_iap) {
            session->config.stop_iap(session->config.media_context);
            session->iap_port = 0;
        }
        if (screen_selected && session->screen_started) {
            if (session->config.stop_screen != NULL) {
                session->config.stop_screen(session->config.media_context);
            }
            session->screen_started = false;
            session->record_active = false;
            session->screen_port = 0U;
        }
        for (size_t index = 0U; index < AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS;
             ++index) {
            if ((audio_type_mask & (uint8_t)(1U << index)) == 0U) continue;
            session_stop_audio_slot(session, (unsigned)index);
        }
        return status_response(request, AIRPLAY_RTSP_STATUS_OK, "OK",
                               response, response_capacity, response_len);
    }

    if (session->iap_port && session->config.stop_iap) {
        session->config.stop_iap(session->config.media_context);
        session->iap_port = 0;
    }
    session_stop_all_audio(session);
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
    uint8_t audio_type_mask = 0U;
    uint64_t audio_connection_ids[AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS] = {0};
    uint64_t audio_format_bits[AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS] = {0};
    airplay_audio_options_t audio_options[AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS] = {0};
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
        if (candidate_type == 130U && session->config.start_iap != NULL) {
            airplay_bplist_object_t o;
            char uuid[40]; size_t uuid_len = 0;
            uint64_t seed = 0, connection = 0;
            bool has_connection = false;
            if (stream_count != 1U ||
                airplay_bplist_dict_get(&doc, entry, "clientTypeUUID", &o) != AIRPLAY_BPLIST_OK ||
                airplay_bplist_get_string(&doc, o, uuid, sizeof(uuid), &uuid_len) != AIRPLAY_BPLIST_OK ||
                (uuid_len != 36U || strlen(uuid) != 36U ||
                 !path_has_suffix(uuid, "E9459FD0-BCAD-4C45-820F-1E72447EF2F2")) ||
                airplay_bplist_dict_get(&doc, entry, "seed", &o) != AIRPLAY_BPLIST_OK ||
                airplay_bplist_get_uint(&doc, o, &seed) != AIRPLAY_BPLIST_OK)
                return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
            airplay_bplist_result_t got = airplay_bplist_dict_get(&doc, entry, "streamConnectionID", &o);
            if (got == AIRPLAY_BPLIST_OK) {
                if (airplay_bplist_get_uint(&doc, o, &connection) != AIRPLAY_BPLIST_OK)
                    return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
                has_connection = true;
            } else if (got != AIRPLAY_BPLIST_NOT_FOUND)
                return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
            if (!session->initial_setup_complete ||
                (session->iap_started && (session->iap_seed != seed || !session->iap_port)))
                return status_response(request, 503, "Service Unavailable", response, response_capacity, response_len);
            if (!session->iap_started) {
                if (data_stream_connection_id_was_used(session, seed) ||
                    session->data_stream_connection_id_count >= AIRPLAY_RTSP_SESSION_MAX_DATA_STREAM_KEY_IDS)
                    return status_response(request, 503, "Service Unavailable", response, response_capacity, response_len);
                uint16_t port = 0;
                if (!session->config.start_iap(session->pairing, seed, &port, session->config.media_context) || !port)
                    return status_response(request, 503, "Service Unavailable", response, response_capacity, response_len);
                (void)remember_data_stream_connection_id(session, seed);
                session->iap_started = true;
                session->iap_seed = seed;
                session->iap_port = port;
                RTCP_LOGI(TAG, "iap_setup=ready port=%u stream_id=1", (unsigned)port);
            }
            airplay_bplist_stream_entry_t result_entry = {
                .stream_type=130, .data_port=session->iap_port, .stream_id=1,
                .has_connection_id=has_connection, .stream_connection_id=connection};
            return streams_setup_response(request, &result_entry, 1, response, response_capacity, response_len);
        }
        RTCP_LOGI(TAG, "setup=stream_candidate index=%u type=0x%08x%08x",
                 (unsigned)index, (unsigned)(candidate_type >> 32),
                 (unsigned)(candidate_type & UINT32_MAX));
        if (candidate_type == 110U) {
            if (has_screen_stream) {
                return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                                       "Bad Request", response, response_capacity,
                                       response_len);
            }
            has_screen_stream = true;
            screen_stream = entry;
            continue;
        }
        if (!airplay_audio_stream_type_is_supported(candidate_type)) {
            /* Unsupported stream types are omitted from the response, like
             * playport's handleStreams() does. */
            continue;
        }
        uint16_t audio_type = (uint16_t)candidate_type;
        char role[24]="default";size_t role_len=0;
        if(airplay_bplist_dict_get(&doc,entry,"audioType",&type_object)==AIRPLAY_BPLIST_OK &&
           (airplay_bplist_get_string(&doc,type_object,role,sizeof(role),&role_len)!=AIRPLAY_BPLIST_OK || !role_len || strlen(role)!=role_len))
            return status_response(request,400,"Bad Request",response,response_capacity,response_len);
        int slot = airplay_audio_slot_for(audio_type,role);
        if (slot < 0 || (audio_type_mask & (uint8_t)(1U << slot)) != 0U) {
            return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                                   "Bad Request", response, response_capacity,
                                   response_len);
        }
        uint64_t connection_id = 0U;
        uint64_t format_bits = 0U;
        if (airplay_bplist_dict_get(&doc, entry, "streamConnectionID",
                                    &type_object) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_uint(&doc, type_object, &connection_id) !=
                AIRPLAY_BPLIST_OK ||
            airplay_bplist_dict_get(&doc, entry, "audioFormat", &type_object) !=
                AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_uint(&doc, type_object, &format_bits) !=
                AIRPLAY_BPLIST_OK) {
            RTCP_LOGW(TAG, "setup=audio_missing_fields type=%u", (unsigned)audio_type);
            return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                                   "Bad Request", response, response_capacity,
                                   response_len);
        }
        airplay_audio_options_t *opt = &audio_options[slot];
        opt->connection_id = connection_id; opt->format_bits = format_bits;
        strcpy(opt->audio_type, "default");
        size_t text_len = 0;
        if (airplay_bplist_dict_get(&doc, entry, "audioType", &type_object) == AIRPLAY_BPLIST_OK &&
            (airplay_bplist_get_string(&doc, type_object, opt->audio_type, sizeof(opt->audio_type), &text_len) != AIRPLAY_BPLIST_OK ||
             !text_len || strlen(opt->audio_type) != text_len))
            return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
        uint64_t value = 0;
        if (airplay_bplist_dict_get(&doc, entry, "dataPort", &type_object) == AIRPLAY_BPLIST_OK) {
            if (airplay_bplist_get_uint(&doc, type_object, &value) != AIRPLAY_BPLIST_OK || value > 65535)
                return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
            opt->input_port = (uint16_t)value;
        }
        if (airplay_bplist_dict_get(&doc, entry, "framesPerPacket", &type_object) == AIRPLAY_BPLIST_OK) {
            if (airplay_bplist_get_uint(&doc, type_object, &value) != AIRPLAY_BPLIST_OK || value > 2880)
                return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
            opt->frames_per_packet = (uint16_t)value;
        }
        bool requested = opt->input_port != 0;
        if (airplay_bplist_dict_get(&doc, entry, "input", &type_object) == AIRPLAY_BPLIST_OK) {
            if (airplay_bplist_get_bool(&doc, type_object, &requested) != AIRPLAY_BPLIST_OK) {
                if (airplay_bplist_get_uint(&doc, type_object, &value) != AIRPLAY_BPLIST_OK || value > 1)
                    return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
                requested = value != 0;
            }
        }
        bool speech = strcmp(opt->audio_type, "telephony") == 0 || strcmp(opt->audio_type, "speechRecognition") == 0;
        opt->input = audio_type == 100 && (requested || speech) && opt->input_port != 0;
        if (requested && !opt->input_port)
            return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
        if (opt->input && !session->config.configure_audio)
            return status_response(request, 503, "Service Unavailable", response, response_capacity, response_len);
        audio_type_mask |= (uint8_t)(1U << slot);
        audio_connection_ids[slot] = connection_id;
        audio_format_bits[slot] = format_bits;
    }

    bool screen_accept = false;
    bool screen_start_now = false;
    uint64_t screen_type = 0U;
    uint64_t screen_connection_id = 0U;
    uint16_t screen_port = 0U;
    if (has_screen_stream) {
        airplay_bplist_object_t type_object;
        if (airplay_bplist_dict_get(&doc, screen_stream, "type", &type_object) !=
                AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_uint(&doc, type_object, &screen_type) !=
                AIRPLAY_BPLIST_OK ||
            airplay_bplist_dict_get(&doc, screen_stream, "streamConnectionID",
                                    &type_object) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_uint(&doc, type_object, &screen_connection_id) !=
                AIRPLAY_BPLIST_OK) {
            return status_response(request, AIRPLAY_RTSP_STATUS_BAD_REQUEST,
                                   "Bad Request", response, response_capacity,
                                   response_len);
        }
        if (session->screen_started &&
            screen_connection_id == session->active_screen_connection_id) {
            screen_accept = true;
            screen_port = session->screen_port;
        } else if (data_stream_connection_id_was_used(session,
                                                      screen_connection_id) ||
                   session->data_stream_connection_id_count >=
                       AIRPLAY_RTSP_SESSION_MAX_DATA_STREAM_KEY_IDS) {
            return status_response(request, AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                                   "Service Unavailable", response,
                                   response_capacity, response_len);
        } else {
            screen_accept = true;
            screen_start_now = true;
        }
    }

    bool audio_accept[AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS] = {false};
    bool audio_start_now[AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS] = {false};
    uint16_t audio_data_port[AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS] = {0};
    uint16_t audio_control_port[AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS] = {0};
    for (size_t slot = 0U; slot < AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS; ++slot) {
        if ((audio_type_mask & (uint8_t)(1U << slot)) == 0U) continue;
        uint16_t stream_type = airplay_audio_slot_type((unsigned)slot);
        /* Legacy callbacks identify streams only by type. Keep them safe by
         * allowing one role per type unless both extended callbacks exist. */
        if (!session->config.start_audio_ex || !session->config.stop_audio_ex) {
            for (unsigned other=0; other<AIRPLAY_AUDIO_MAX_STREAMS; ++other) {
                if (other==slot || airplay_audio_slot_type(other)!=stream_type) continue;
                if (session->audio[other].started || (audio_type_mask & (1U<<other)))
                    return status_response(request,503,"Service Unavailable",response,response_capacity,response_len);
            }
        }
        session_audio_stream_t *existing = session->audio[slot].started ? &session->audio[slot] : NULL;
        if (existing != NULL &&
            existing->connection_id == audio_connection_ids[slot]) {
            if (memcmp(&existing->options, &audio_options[slot], sizeof(existing->options)) != 0)
                return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
            audio_accept[slot] = true;
            audio_data_port[slot] = existing->data_port;
            audio_control_port[slot] = existing->control_port;
            continue;
        }
        if ((session->config.start_audio == NULL && session->config.start_audio_ex == NULL) ||
            data_stream_connection_id_was_used(session,
                                               audio_connection_ids[slot]) ||
            session->data_stream_connection_id_count >=
                AIRPLAY_RTSP_SESSION_MAX_DATA_STREAM_KEY_IDS) {
            RTCP_LOGW(TAG, "audio_setup=refused type=%u connection_id=%llu "
                           "start=%u reused=%u",
                      (unsigned)stream_type,
                      (unsigned long long)audio_connection_ids[slot],
                      (session->config.start_audio != NULL || session->config.start_audio_ex != NULL) ? 1U : 0U,
                      data_stream_connection_id_was_used(
                          session, audio_connection_ids[slot])
                          ? 1U
                          : 0U);
            return status_response(request, AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                                   "Service Unavailable", response,
                                   response_capacity, response_len);
        }
        audio_accept[slot] = true;
        audio_start_now[slot] = true;
    }

    /* Validate the complete transaction before starting any endpoint. Reserve
     * keys before invoking callbacks: an input worker may transmit immediately. */
    size_t new_keys = screen_start_now ? 1U : 0U;
    for (size_t i = 0; i < AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS; ++i) {
        if (!(audio_type_mask & (1U << i))) continue;
        if (has_screen_stream && audio_connection_ids[i] == screen_connection_id)
            return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
        for (size_t j = 0; j < i; ++j)
            if ((audio_type_mask & (1U << j)) && audio_connection_ids[i] == audio_connection_ids[j])
                return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
        if (audio_start_now[i]) ++new_keys;
    }
    if (new_keys > AIRPLAY_RTSP_SESSION_MAX_DATA_STREAM_KEY_IDS - session->data_stream_connection_id_count)
        return status_response(request, 503, "Service Unavailable", response, response_capacity, response_len);
    if (screen_start_now) (void)remember_data_stream_connection_id(session, screen_connection_id);
    for (size_t i = 0; i < AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS; ++i)
        if (audio_start_now[i]) (void)remember_data_stream_connection_id(session, audio_connection_ids[i]);
    bool started_screen_now = false;
    if (screen_start_now) {
        if (session->screen_started && session->config.stop_screen != NULL) {
            session->config.stop_screen(session->config.media_context);
        }
        session->screen_started = false;
        session->screen_port = 0U;
        uint16_t bound_port = 0U;
        bool started = session->config.start_screen != NULL &&
            session->config.start_screen(session->pairing, screen_connection_id,
                                          session->config.screen_requested_port,
                                          &bound_port,
                                          session->config.media_context);
        if (!started || bound_port == 0U) {
            RTCP_LOGE(TAG, "screen_start=failed stream_type=%llu connection_id=%llu port=%u",
                     (unsigned long long)screen_type,
                     (unsigned long long)screen_connection_id,
                     (unsigned)bound_port);
            if (started && session->config.stop_screen != NULL) {
                session->config.stop_screen(session->config.media_context);
            }
            return status_response(request, AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                                   "Service Unavailable", response,
                                   response_capacity, response_len);
        }
        session->screen_started = true;
        session->screen_port = bound_port;
        session->active_screen_connection_id = screen_connection_id;
        screen_port = bound_port;
        started_screen_now = true;
        RTCP_LOGI(TAG, "screen_start=ready port=%u stream_type=0x%08x%08x connection_id=0x%08x%08x",
                 (unsigned)bound_port, (unsigned)(screen_type >> 32),
                 (unsigned)(screen_type & UINT32_MAX),
                 (unsigned)(screen_connection_id >> 32),
                 (unsigned)(screen_connection_id & UINT32_MAX));
    }

    uint8_t started_audio_mask = 0U;
    bool audio_failed = false;
    for (size_t slot = 0U; slot < AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS; ++slot) {
        if (!audio_start_now[slot]) continue;
        uint16_t stream_type = airplay_audio_slot_type((unsigned)slot);
        session_stop_audio_slot(session, (unsigned)slot);
        uint16_t data_port = 0U;
        uint16_t control_port = 0U;
        bool started = session->config.start_audio_ex ?
            session->config.start_audio_ex(session->pairing,stream_type,&audio_options[slot],&data_port,&control_port,session->config.media_context) :
            session->config.start_audio && session->config.start_audio(session->pairing,stream_type,audio_connection_ids[slot],audio_format_bits[slot],&data_port,&control_port,session->config.media_context);
        if (!started || data_port == 0U || control_port == 0U) {
            RTCP_LOGE(TAG, "audio_start=failed type=%u connection_id=%llu "
                           "data_port=%u control_port=%u format=0x%llx",
                     (unsigned)stream_type,
                     (unsigned long long)audio_connection_ids[slot],
                     (unsigned)data_port, (unsigned)control_port,
                     (unsigned long long)audio_format_bits[slot]);
            if(started)stop_audio_endpoint(session,stream_type,&audio_options[slot]);
            audio_failed = true;
            break;
        }
        if (session->config.configure_audio &&
            !session->config.configure_audio(session->pairing, stream_type, &audio_options[slot], session->config.media_context)) {
            stop_audio_endpoint(session,stream_type,&audio_options[slot]);
            audio_failed = true; break;
        }
        session_audio_stream_t *target = &session->audio[slot];
        if (target == NULL) {
            stop_audio_endpoint(session,stream_type,&audio_options[slot]);
            audio_failed = true;
            break;
        }
        target->started = true;
        target->stream_type = stream_type;
        target->data_port = data_port;
        target->control_port = control_port;
        target->connection_id = audio_connection_ids[slot];
        target->options = audio_options[slot];
        audio_data_port[slot] = data_port;
        audio_control_port[slot] = control_port;
        started_audio_mask |= (uint8_t)(1U << slot);
        RTCP_LOGI(TAG, "audio_start=ready type=%u connection_id=%llu "
                       "data_port=%u control_port=%u format=0x%llx",
                 (unsigned)stream_type,
                 (unsigned long long)audio_connection_ids[slot],
                 (unsigned)data_port, (unsigned)control_port,
                 (unsigned long long)audio_format_bits[slot]);
    }

    if (audio_failed) {
        for (size_t slot = 0U; slot < AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS;
             ++slot) {
            if ((started_audio_mask & (uint8_t)(1U << slot)) != 0U) {
                session_stop_audio_slot(session,(unsigned)slot);
            }
        }
        if (started_screen_now) {
            if (session->config.stop_screen != NULL) {
                session->config.stop_screen(session->config.media_context);
            }
            session->screen_started = false;
            session->screen_port = 0U;
        }
        return status_response(request, AIRPLAY_RTSP_STATUS_SESSION_NOT_READY,
                               "Service Unavailable", response,
                               response_capacity, response_len);
    }

    airplay_bplist_stream_entry_t entries[AIRPLAY_BPLIST_MAX_SETUP_STREAMS];
    size_t entry_count = 0U;
    memset(entries, 0, sizeof(entries));
    if (screen_accept) {
        entries[entry_count].stream_type = screen_type;
        entries[entry_count].data_port = screen_port;
        ++entry_count;
    }
    for (size_t slot = 0U; slot < AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS; ++slot) {
        if (!audio_accept[slot]) continue;
        entries[entry_count].stream_type =
            (uint64_t)airplay_audio_slot_type((unsigned)slot);
        entries[entry_count].data_port = audio_data_port[slot];
        entries[entry_count].control_port = audio_control_port[slot];
        entries[entry_count].has_connection_id = true;
        entries[entry_count].stream_connection_id = audio_connection_ids[slot];
        ++entry_count;
    }
    if (entry_count == 0U) {
        RTCP_LOGW(TAG, "setup=streams_without_accepted count=%u",
                  (unsigned)stream_count);
    }
    result = streams_setup_response(request, entries, entry_count, response,
                                    response_capacity, response_len);
    if (result != AIRPLAY_RTSP_SESSION_OK) {
        for (size_t slot = 0U; slot < AIRPLAY_RTSP_SESSION_MAX_AUDIO_STREAMS;
             ++slot) {
            if ((started_audio_mask & (uint8_t)(1U << slot)) != 0U) {
                session_stop_audio_slot(session,(unsigned)slot);
            }
        }
        if (started_screen_now && session->config.stop_screen != NULL) {
            session->config.stop_screen(session->config.media_context);
            session->screen_started = false;
            session->screen_port = 0U;
        }
    }
    return result;
}

/* Parses a "volume: <dB>" line out of a text/parameters SET_PARAMETER body.
 * The body is a borrow into the receive buffer, so it is not NUL-terminated
 * and is scanned line by line.  Values are clamped to the AirPlay scale
 * [-144, 0] dB. */
static bool parse_volume_parameter(const uint8_t *body, size_t body_len,
                                   float *volume_db)
{
    static const char name[] = "volume:";
    const size_t name_len = sizeof(name) - 1U;
    size_t offset = 0U;
    while (offset < body_len) {
        size_t line_end = offset;
        while (line_end < body_len && body[line_end] != '\r' &&
               body[line_end] != '\n') {
            ++line_end;
        }
        if (line_end - offset >= name_len &&
            memcmp(body + offset, name, name_len) == 0) {
            const uint8_t *cursor = body + offset + name_len;
            const uint8_t *end = body + line_end;
            while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
                ++cursor;
            }
            bool negative = false;
            if (cursor < end && (*cursor == '-' || *cursor == '+')) {
                negative = *cursor == '-';
                ++cursor;
            }
            bool digits = false;
            double value = 0.0;
            while (cursor < end && *cursor >= '0' && *cursor <= '9') {
                value = value * 10.0 + (double)(*cursor - '0');
                digits = true;
                ++cursor;
            }
            if (cursor < end && *cursor == '.') {
                ++cursor;
                double scale = 0.1;
                while (cursor < end && *cursor >= '0' && *cursor <= '9') {
                    value += (double)(*cursor - '0') * scale;
                    scale *= 0.1;
                    digits = true;
                    ++cursor;
                }
            }
            if (!digits) return false;
            if (negative) value = -value;
            if (value > 0.0) value = 0.0;
            if (value < -144.0) value = -144.0;
            *volume_db = (float)value;
            return true;
        }
        while (line_end < body_len && body[line_end] != '\n') ++line_end;
        offset = line_end + 1U;
    }
    return false;
}

static void handle_set_parameter(airplay_rtsp_session_t *session,
                                 const airplay_rtsp_request_t *request)
{
    if (session->config.on_volume == NULL) return;
    float volume_db = 0.0f;
    if (!parse_volume_parameter(request->body, request->body_len,
                                &volume_db)) {
        return;
    }
    session->config.on_volume(volume_db, session->config.volume_context);
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

static bool parse_audio_modes(const airplay_bplist_doc_t *doc, airplay_bplist_object_t params,
                              airplay_audio_modes_t *modes)
{
    memset(modes,0,sizeof(*modes));
    const char *keys[] = {"appStates", "resources"};
    for(unsigned k=0;k<2;++k) {
        airplay_bplist_object_t array, entry, object;
        airplay_bplist_type_t type;uint64_t count;
        airplay_bplist_result_t rc=airplay_bplist_dict_get(doc,params,keys[k],&array);
        if(rc==AIRPLAY_BPLIST_NOT_FOUND)continue;
        if(rc!=AIRPLAY_BPLIST_OK || airplay_bplist_get_type(doc,array,&type,&count)!=AIRPLAY_BPLIST_OK ||
           type!=AIRPLAY_BPLIST_ARRAY || count>16)return false;
        for(uint64_t i=0;i<count;++i) {
            uint64_t id,value;
            if(airplay_bplist_array_get(doc,array,i,&entry)!=AIRPLAY_BPLIST_OK ||
               airplay_bplist_dict_get(doc,entry,k?"resourceID":"appStateID",&object)!=AIRPLAY_BPLIST_OK ||
               airplay_bplist_get_uint(doc,object,&id)!=AIRPLAY_BPLIST_OK)return false;
            if(!k && id==1) {
                if(airplay_bplist_dict_get(doc,entry,"speechMode",&object)==AIRPLAY_BPLIST_OK) {
                    double mode;
                    if(airplay_bplist_get_number(doc,object,&mode)!=AIRPLAY_BPLIST_OK || mode< -1 || mode>3 || mode!=(int)mode)return false;
                    modes->has_speech=true;modes->speech_mode=(int)mode;
                }
            } else if((!k && id==2) || (k && id==2)) {
                if(airplay_bplist_dict_get(doc,entry,"entity",&object)!=AIRPLAY_BPLIST_OK)continue;
                if(airplay_bplist_get_uint(doc,object,&value)!=AIRPLAY_BPLIST_OK || value>3)return false;
                if(k) {modes->has_main_audio=true;modes->main_audio_entity=(uint32_t)value;}
                else {modes->has_call=true;modes->call_active=value==1;}
            }
        }
    }
    return true;
}

/* One bounded trace per controller, using the document actually served.
 * No pairing keys, certificates or arbitrary request bodies are logged. */
static void log_info_audio(airplay_rtsp_session_t *session)
{
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root, array, entry, object;
    airplay_bplist_type_t type;
    uint64_t count = 0, features = 0;
    if (airplay_bplist_open(session->config.info_plist, session->config.info_plist_len, &doc) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_root(&doc, &root) != AIRPLAY_BPLIST_OK) return;
    if (airplay_bplist_dict_get(&doc, root, "features", &object) == AIRPLAY_BPLIST_OK)
        (void)airplay_bplist_get_uint(&doc, object, &features);
    if (airplay_bplist_dict_get(&doc, root, "audioFormats", &array) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_type(&doc, array, &type, &count) != AIRPLAY_BPLIST_OK || type != AIRPLAY_BPLIST_ARRAY)
        count = 0;
    RTCP_LOGI(TAG, "info=served bytes=%u features=0x%llx audio_formats=%u",
              (unsigned)session->config.info_plist_len, (unsigned long long)features, (unsigned)count);
    for (uint64_t i = 0; i < count && i < 16; ++i) {
        uint64_t stream_type = 0, output = 0, input = 0;
        char role[32] = "any";
        size_t length = 0;
        if (airplay_bplist_array_get(&doc, array, i, &entry) != AIRPLAY_BPLIST_OK) continue;
        if (airplay_bplist_dict_get(&doc, entry, "type", &object) == AIRPLAY_BPLIST_OK)
            (void)airplay_bplist_get_uint(&doc, object, &stream_type);
        if (airplay_bplist_dict_get(&doc, entry, "audioType", &object) == AIRPLAY_BPLIST_OK)
            (void)airplay_bplist_get_string(&doc, object, role, sizeof(role), &length);
        if (airplay_bplist_dict_get(&doc, entry, "audioOutputFormats", &object) == AIRPLAY_BPLIST_OK)
            (void)airplay_bplist_get_uint(&doc, object, &output);
        if (airplay_bplist_dict_get(&doc, entry, "audioInputFormats", &object) == AIRPLAY_BPLIST_OK)
            (void)airplay_bplist_get_uint(&doc, object, &input);
        RTCP_LOGI(TAG, "info_audio type=%u role=%s output=0x%llx input=0x%llx",
                  (unsigned)stream_type, role, (unsigned long long)output, (unsigned long long)input);
    }
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
        if (!session->info_logged) {
            log_info_audio(session);
            session->info_logged = true;
        }
        return build_response(request, AIRPLAY_RTSP_STATUS_OK, "OK",
                              AIRPLAY_PLIST_CONTENT_TYPE,
                              session->config.info_plist,
                              session->config.info_plist_len,
                              response, response_capacity, response_len);
    }

    if(strcmp(request->method,"POST")==0 && path_has_suffix(request->path,"/feedback")) {
        if(!session->initial_setup_complete)
            return status_response(request,503,"Service Unavailable",response,response_capacity,response_len);
        if(request->body_len) {
            airplay_bplist_doc_t doc;airplay_bplist_object_t root;airplay_bplist_type_t type;
            if(airplay_bplist_open(request->body,request->body_len,&doc)!=AIRPLAY_BPLIST_OK ||
               airplay_bplist_root(&doc,&root)!=AIRPLAY_BPLIST_OK ||
               airplay_bplist_get_type(&doc,root,&type,NULL)!=AIRPLAY_BPLIST_OK || type!=AIRPLAY_BPLIST_DICTIONARY)
                return status_response(request,400,"Bad Request",response,response_capacity,response_len);
        }
        airplay_audio_feedback_t entries[AIRPLAY_AUDIO_MAX_STREAMS];size_t count=0;
        for(unsigned i=0;i<AIRPLAY_AUDIO_MAX_STREAMS;++i) {
            session_audio_stream_t *slot=&session->audio[i];rtcp_audio_format_t format;
            if(slot->started && rtcp_audio_format_parse(slot->options.format_bits,&format))
                entries[count++]=(airplay_audio_feedback_t){slot->stream_type,format.clock_rate,slot->connection_id};
        }
        uint8_t body[2048];size_t len=0;
        if(airplay_bplist_build_audio_feedback(entries,count,body,sizeof(body),&len)!=AIRPLAY_BPLIST_OK)
            return status_response(request,500,"Internal Server Error",response,response_capacity,response_len);
        return build_response(request,200,"OK",AIRPLAY_PLIST_CONTENT_TYPE,body,len,response,response_capacity,response_len);
    }
    if (strcmp(request->method, "POST") == 0 &&
        path_has_suffix(request->path, "/command")) {
        airplay_bplist_doc_t doc;
        airplay_bplist_object_t root, object, params;
        airplay_bplist_type_t type;
        char command[64]; size_t length = 0;
        if (airplay_bplist_open(request->body, request->body_len, &doc) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_root(&doc, &root) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_dict_get(&doc, root, "type", &object) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_string(&doc, object, command, sizeof(command), &length) != AIRPLAY_BPLIST_OK ||
            strlen(command) != length ||
            airplay_bplist_dict_get(&doc, root, "params", &params) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_type(&doc, params, &type, NULL) != AIRPLAY_BPLIST_OK || type != AIRPLAY_BPLIST_DICTIONARY)
            return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
        if (!session->initial_setup_complete)
            return status_response(request, 503, "Service Unavailable", response, response_capacity, response_len);
        if (strcmp(command, "modesChanged") == 0) {
            airplay_audio_modes_t modes;
            if(!parse_audio_modes(&doc,params,&modes))
                return status_response(request,400,"Bad Request",response,response_capacity,response_len);
            if(session->config.on_audio_modes)session->config.on_audio_modes(&modes,session->config.command_context);
            ++session->modes_changes;
            if (session->modes_changes <= 2U)
                RTCP_LOGI(TAG, "command=modesChanged acknowledged count=%u", session->modes_changes);
            return status_response(request, 200, "OK", response, response_capacity, response_len);
        }
        if (strcmp(command,"duckAudio")==0 || strcmp(command,"unduckAudio")==0) {
            bool ducked=strcmp(command,"duckAudio")==0;
            double volume=ducked?-12.0:0.0, duration=100.0;
            if(airplay_bplist_dict_get(&doc,params,"volume",&object)==AIRPLAY_BPLIST_OK &&
               airplay_bplist_get_number(&doc,object,&volume)!=AIRPLAY_BPLIST_OK)
                return status_response(request,400,"Bad Request",response,response_capacity,response_len);
            if(airplay_bplist_dict_get(&doc,params,"durationMs",&object)==AIRPLAY_BPLIST_OK &&
               airplay_bplist_get_number(&doc,object,&duration)!=AIRPLAY_BPLIST_OK)
                return status_response(request,400,"Bad Request",response,response_capacity,response_len);
            if(volume< -144 || volume>0 || duration<0 || duration>10000)
                return status_response(request,400,"Bad Request",response,response_capacity,response_len);
            if(!session->config.on_audio_duck || !session->config.on_audio_duck(ducked,(float)volume,(uint32_t)duration,session->config.command_context))
                return status_response(request,503,"Service Unavailable",response,response_capacity,response_len);
            return status_response(request,200,"OK",response,response_capacity,response_len);
        }
        if (strcmp(command, "disableBluetooth") == 0) {
            char device[64];
            if (airplay_bplist_dict_get(&doc, params, "deviceID", &object) != AIRPLAY_BPLIST_OK ||
                airplay_bplist_get_string(&doc, object, device, sizeof(device), &length) != AIRPLAY_BPLIST_OK ||
                length == 0U || strlen(device) != length)
                return status_response(request, 400, "Bad Request", response, response_capacity, response_len);
            if (!session->config.disable_bluetooth ||
                !session->config.disable_bluetooth(device, session->config.command_context))
                return status_response(request, 503, "Service Unavailable", response, response_capacity, response_len);
            return status_response(request, 200, "OK", response, response_capacity, response_len);
        }
        if (++session->unsupported_commands <= 8U)
            RTCP_LOGI(TAG, "unsupported_command bytes=%u count=%u", (unsigned)request->body_len, session->unsupported_commands);
        return status_response(request, 405, "Method Not Allowed", response, response_capacity, response_len);
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
    if (strcmp(request->method, "SET_PARAMETER") == 0) {
        /* Volume rides on SET_PARAMETER while the session is live; unknown
         * parameters (progress, ...) are acknowledged and ignored. */
        handle_set_parameter(session, request);
        return status_response(request, AIRPLAY_RTSP_STATUS_OK, "OK",
                               response, response_capacity, response_len);
    }
    if (strcmp(request->method, "OPTIONS") == 0 ||
        strcmp(request->method, "GET_PARAMETER") == 0) {
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
    if (session->iap_port && session->config.stop_iap) {
        session->config.stop_iap(session->config.media_context);
        session->iap_port = 0;
    }
    session_stop_all_audio(session);
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

void airplay_rtsp_session_iap_closed(airplay_rtsp_session_t *session)
{
    if (session) session->iap_port = 0;
}
