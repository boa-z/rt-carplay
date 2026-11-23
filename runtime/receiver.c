/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtcp/receiver.h"
#include "airplay_rtsp_server.h"
#include "rtcp/receiver_compat.h"
#include "receiver_vehicle.h"
#include "airplay_info_plist.h"
#include "input_bridge.h"
#include <stdlib.h>
#include <string.h>

struct rtcp_receiver
{
    airplay_rtsp_server_config_t airplay;
    uint8_t info[AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES];
    airplay_rtsp_server_t *server;
    rtcp_receiver_event_cb_t on_state;
    void *context;
    rtcp_receiver_state_t state;
    bool busy;
    bool has_volume;
    float volume_db;
    rtcp_now_playing_cb_t on_now_playing;
    void *now_playing_context;
    rtcp_now_playing_details_cb_t on_now_playing_details;
    void *now_playing_details_context;
    rtcp_host_ui_cb_t on_host_ui;
    void *host_ui_context;
    receiver_vehicle_t *vehicle;
    rtcp_data_stream_backend_t raw_data_stream;
    rtcp_ui_sync_t *ui_sync;
    rtcp_ui_sync_delivery_status_t ui_delivery;
    rtcp_vehicle_t *vehicle_model;
    bool (*host_info)(const uint8_t **, size_t *, void *);
    void *host_info_context;
};

static bool receiver_volume(float *volume_db, void *context)
{
    const rtcp_receiver_t *receiver = context;
    if (!receiver->has_volume)
        return false;
    *volume_db = receiver->volume_db;
    return true;
}

static void receiver_now_playing(const rtcp_now_playing_t *update, void *context)
{
    const rtcp_receiver_t *receiver = context;
    if (receiver->on_now_playing)
        receiver->on_now_playing(update, receiver->now_playing_context);
}

static void receiver_now_playing_details(const rtcp_now_playing_details_t *update, void *context)
{
    rtcp_receiver_t *receiver = context;
    if (receiver->on_now_playing_details)
        receiver->on_now_playing_details(update, receiver->now_playing_details_context);
}

static bool receiver_host_ui(void *context)
{
    const rtcp_receiver_t *receiver = context;
    return receiver->on_host_ui && receiver->on_host_ui(receiver->host_ui_context);
}

static bool receiver_info(const uint8_t **bytes, size_t *length, void *context)
{
    rtcp_receiver_t *r = context;
    if (r->host_info)
    {
        if (!r->host_info(bytes, length, r->host_info_context))
            return false;
    }
    else
    {
        *bytes = r->info;
        *length = r->airplay.info_plist_len;
    }
    return (!r->airplay.negotiate_vehicle || airplay_info_plist_matches_vehicle(*bytes, *length, r->vehicle_model)) &&
           (!r->airplay.negotiate_ui_sync || airplay_info_plist_matches_ui_sync(*bytes, *length, r->ui_sync));
}

static int ui_flush(rtcp_receiver_t *r)
{
    if (!r->ui_sync)
        return RTCP_OK;
    const uint8_t *body;
    size_t length;
    while (rtcp_ui_sync_peek(r->ui_sync, &body, &length) == RTCP_OK)
    {
        int rc = airplay_rtsp_server_send_data_stream(r->server, RTCP_DATA_STREAM_UI_SYNC, body, length);
        if (rc)
        {
            r->ui_delivery.last_error = rc;
            if (rc == RTCP_ENOMEM)
                ++r->ui_delivery.queue_retries;
            return rc;
        }
        ++r->ui_delivery.queued_messages;
        (void)rtcp_ui_sync_consume(r->ui_sync);
    }
    return RTCP_OK;
}

static int vehicle_send(void *context, rtcp_data_stream_kind_t kind,
                        const uint8_t *body, size_t length)
{
    rtcp_receiver_t *r = context;
    return airplay_rtsp_server_send_data_stream(r->server, kind, body, length);
}
static bool receiver_data_body(rtcp_data_stream_kind_t kind,
                               const uint8_t *body, size_t length, void *context)
{
    rtcp_receiver_t *r = context;
    if (r->vehicle && (RTCP_RECEIVER_VEHICLE_MASK & RTCP_DATA_STREAM_MASK(kind)))
        return receiver_vehicle_body(r->vehicle, kind, body, length);
    if (r->ui_sync && kind == RTCP_DATA_STREAM_UI_SYNC)
    {
        int rc = ui_flush(r);
        if (!rc)
            rc = rtcp_ui_sync_handle(r->ui_sync, body, length);
        if (rc)
        {
            ++r->ui_delivery.rejected_requests;
            r->ui_delivery.last_error = r->ui_delivery.last_rejection = rc;
            return false;
        }
        rc = ui_flush(r);
        return rc == RTCP_OK || rc == RTCP_ENOMEM;
    }
    return r->raw_data_stream.on_body && r->raw_data_stream.on_body(kind, body, length,
                                                                    r->raw_data_stream.context);
}
static void receiver_data_closed(rtcp_data_stream_kind_t kind, int error, void *context)
{
    rtcp_receiver_t *r = context;
    if (r->vehicle && (RTCP_RECEIVER_VEHICLE_MASK & RTCP_DATA_STREAM_MASK(kind)))
        receiver_vehicle_closed(r->vehicle, kind, error);
    else if (r->ui_sync && kind == RTCP_DATA_STREAM_UI_SYNC)
    {
        rtcp_ui_sync_reset_session(r->ui_sync);
        r->ui_delivery.last_error = error;
    }
    else if (r->raw_data_stream.on_closed)
        r->raw_data_stream.on_closed(kind, error, r->raw_data_stream.context);
}

/* An intentional Null Sink lets a headless host exercise the full protocol. */
static int discard_screen(airplay_screen_message_type_t type, uint8_t *bytes,
                          size_t length, size_t prefix, uint64_t sender_ns,
                          uint64_t arrival_us, void *context)
{
    (void)type;
    (void)bytes;
    (void)length;
    (void)prefix;
    (void)sender_ns;
    (void)arrival_us;
    (void)context;
    return RTCP_OK;
}

static void refresh(rtcp_receiver_t *r, int error)
{
    rtcp_receiver_state_t next = { 0 };
    next.generation = r->state.generation;
    next.last_error = error;
    next.running = r->server != NULL;
    airplay_rtsp_server_get_receiver_state(r->server, &next);
    if (r->state.authenticated && !next.authenticated)
    {
        receiver_vehicle_reset(r->vehicle);
        rtcp_ui_sync_reset_session(r->ui_sync);
    }
    bool changed = next.running != r->state.running ||
                   next.transport_connected != r->state.transport_connected ||
                   next.authenticated != r->state.authenticated ||
                   next.session_established != r->state.session_established ||
                   next.video_ready != r->state.video_ready ||
                   next.audio_ready != r->state.audio_ready ||
                   next.input_ready != r->state.input_ready ||
                   next.last_error != r->state.last_error;
    r->state = next;
    if (changed && r->on_state != NULL)
        r->on_state(&r->state, r->context);
}

int rtcp_receiver_config_from_airplay(const struct airplay_rtsp_server_config *a,
                                      rtcp_receiver_config_t *config)
{
    if (a == NULL || config == NULL)
        return RTCP_EINVAL;
    memset(config, 0, sizeof(*config));
    config->platform.os = a->os;
    config->platform.net = a->net;
    config->platform.pairing = a->pairing;
    config->platform.info_plist = a->info_plist;
    config->platform.info_plist_len = a->info_plist_len;
    config->platform.get_info_plist = a->get_info_plist;
    config->platform.info_context = a->info_context;
    config->port = a->port;
    config->screen_port = a->screen_port;
    config->family = a->family;
    config->screen_max_body_bytes = a->screen_max_body_bytes;
    config->media.on_video = a->on_screen_message;
    config->media.video_context = a->screen_message_context;
    config->media.on_audio = a->on_audio_pcm;
    config->media.audio_context = a->audio_pcm_context;
    config->media.on_audio_stop = a->on_audio_stop;
    config->media.on_audio_stop_ex = a->on_audio_stop_ex;
    config->media.on_audio_input_start = a->on_audio_input_start;
    config->media.on_audio_input_stop = a->on_audio_input_stop;
    config->media.on_audio_modes = a->on_audio_modes;
    config->media.on_audio_duck = a->on_audio_duck;
    config->media.on_volume = a->on_volume;
    config->media.volume_context = a->volume_context;
    config->on_event_state = a->on_event_state;
    config->event_state_context = a->event_state_context;
    config->iap2.on_bytes = a->on_iap_bytes;
    config->iap2.on_tick = a->on_iap_tick;
    config->iap2.on_stop = a->on_iap_stop;
    config->iap2.disable_bluetooth = a->disable_bluetooth;
    config->iap2.context = a->iap_context;
    config->data_stream = a->data_stream;
    config->experimental_alt_video = a->experimental_alt_video;
    config->alt_screen_port = a->alt_screen_port;
    config->media.on_alt_video = a->on_alt_screen_message;
    config->media.alt_video_context = a->alt_screen_message_context;
    return RTCP_OK;
}

int rtcp_receiver_create(const rtcp_receiver_config_t *config,
                         rtcp_receiver_t **out)
{
    if (out == NULL)
        return RTCP_EINVAL;
    *out = NULL;
    if (config == NULL)
        return RTCP_EINVAL;
    const rtcp_platform_t *platform = &config->platform;
    if (platform->os == NULL || platform->net == NULL ||
        platform->info_plist == NULL || platform->info_plist_len == 0 ||
        platform->info_plist_len > AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES ||
        (config->family != 0 && config->family != RTCP_AF_INET &&
         config->family != RTCP_AF_INET6) ||
        (config->data_stream.channel_mask & ~RTCP_DATA_STREAM_ALL_MASK) ||
        (config->data_stream.channel_mask && !config->data_stream.on_body) ||
        (config->vehicle && (config->data_stream.channel_mask & RTCP_RECEIVER_VEHICLE_MASK)) ||
        (config->ui_sync && (config->data_stream.channel_mask & RTCP_DATA_STREAM_MASK(RTCP_DATA_STREAM_UI_SYNC))))
        return RTCP_EINVAL;
    if (config->negotiate_vehicle && (!config->vehicle ||
                                      !airplay_info_plist_matches_vehicle(platform->info_plist, platform->info_plist_len, config->vehicle)))
        return RTCP_EINVAL;
    if (config->negotiate_ui_sync && (!config->ui_sync ||
                                      !airplay_info_plist_matches_ui_sync(platform->info_plist, platform->info_plist_len, config->ui_sync)))
        return RTCP_EINVAL;
    rtcp_receiver_t *r = calloc(1, sizeof(*r));
    if (r == NULL)
        return RTCP_ENOMEM;
    if (config->vehicle)
    {
        r->vehicle = receiver_vehicle_create(config->vehicle, vehicle_send, r);
        if (!r->vehicle)
        {
            free(r);
            return RTCP_ENOMEM;
        }
    }
    r->raw_data_stream = config->data_stream;
    r->ui_sync = config->ui_sync;
    r->ui_delivery.attached = config->ui_sync != NULL;
    r->airplay = (airplay_rtsp_server_config_t){
        .os = config->platform.os,
        .net = config->platform.net,
        .pairing = config->platform.pairing,
        .info_plist = config->platform.info_plist,
        .info_plist_len = config->platform.info_plist_len,
        .get_info_plist = config->platform.get_info_plist,
        .info_context = config->platform.info_context,
        .port = config->port,
        .screen_port = config->screen_port,
        .family = config->family,
        .screen_max_body_bytes = config->screen_max_body_bytes,
        .on_screen_message = config->media.on_video,
        .screen_message_context = config->media.video_context,
        .on_audio_pcm = config->media.on_audio,
        .audio_pcm_context = config->media.audio_context,
        .on_audio_stop = config->media.on_audio_stop,
        .on_audio_stop_ex = config->media.on_audio_stop_ex,
        .on_audio_input_start = config->media.on_audio_input_start,
        .on_audio_input_stop = config->media.on_audio_input_stop,
        .on_audio_modes = config->media.on_audio_modes,
        .on_audio_duck = config->media.on_audio_duck,
        .on_volume = config->media.on_volume,
        .volume_context = config->media.volume_context,
        .get_volume = receiver_volume,
        .get_volume_context = r,
        .on_now_playing = receiver_now_playing,
        .now_playing_context = r,
        .on_now_playing_details = receiver_now_playing_details,
        .now_playing_details_context = r,
        .on_host_ui = receiver_host_ui,
        .host_ui_context = r,
        .on_event_state = config->on_event_state,
        .event_state_context = config->event_state_context,
        .on_iap_bytes = config->iap2.on_bytes,
        .on_iap_tick = config->iap2.on_tick,
        .on_iap_stop = config->iap2.on_stop,
        .disable_bluetooth = config->iap2.disable_bluetooth,
        .iap_context = config->iap2.context,
        .data_stream = config->data_stream,
        .negotiate_vehicle = config->negotiate_vehicle,
        .negotiate_ui_sync = config->negotiate_ui_sync,
        .experimental_alt_video = config->experimental_alt_video,
        .alt_screen_port = config->alt_screen_port,
        .on_alt_screen_message = config->media.on_alt_video,
        .alt_screen_message_context = config->media.alt_video_context,
    };
    if (config->negotiate_vehicle || config->negotiate_ui_sync)
    {
        r->vehicle_model = config->vehicle;
        r->host_info = platform->get_info_plist;
        r->host_info_context = platform->info_context;
        r->airplay.get_info_plist = receiver_info;
        r->airplay.info_context = r;
    }
    if (r->vehicle || r->ui_sync)
    {
        if (r->vehicle)
            r->airplay.data_stream.channel_mask |= RTCP_RECEIVER_VEHICLE_MASK;
        if (r->ui_sync)
            r->airplay.data_stream.channel_mask |= RTCP_DATA_STREAM_MASK(RTCP_DATA_STREAM_UI_SYNC);
        r->airplay.data_stream.on_body = receiver_data_body;
        r->airplay.data_stream.on_closed = receiver_data_closed;
        r->airplay.data_stream.context = r;
    }
    if (r->airplay.on_screen_message == NULL)
        r->airplay.on_screen_message = discard_screen;
    if (r->airplay.experimental_alt_video && !r->airplay.on_alt_screen_message)
        r->airplay.on_alt_screen_message = discard_screen;
    memcpy(r->info, platform->info_plist, platform->info_plist_len);
    r->airplay.info_plist = r->info;
    r->on_state = config->on_state;
    r->context = config->context;
    *out = r;
    return RTCP_OK;
}

int rtcp_receiver_start(rtcp_receiver_t *r)
{
    if (r == NULL)
        return RTCP_EINVAL;
    if (r->busy)
        return RTCP_ESTATE;
    if (r->server != NULL)
        return RTCP_OK;
    r->busy = true;
    receiver_vehicle_reset(r->vehicle);
    rtcp_ui_sync_reset_session(r->ui_sync);
    int result = airplay_rtsp_server_start(&r->airplay, &r->server);
    if (result == RTCP_OK)
        ++r->state.generation;
    refresh(r, result);
    r->busy = false;
    return result;
}

int rtcp_receiver_poll(rtcp_receiver_t *r, int timeout_ms)
{
    if (r == NULL || timeout_ms < 0)
        return RTCP_EINVAL;
    if (r->busy || r->server == NULL)
        return RTCP_ESTATE;
    r->busy = true;
    if (r->state.authenticated)
    {
        receiver_vehicle_tick(r->vehicle);
        (void)ui_flush(r);
    }
    int result = airplay_rtsp_server_poll(r->server, timeout_ms);
    refresh(r, result);
    if (r->state.authenticated)
    {
        receiver_vehicle_tick(r->vehicle);
        (void)ui_flush(r);
    }
    r->busy = false;
    return result;
}

int rtcp_receiver_stop(rtcp_receiver_t *r)
{
    if (r == NULL)
        return RTCP_EINVAL;
    if (r->busy)
        return RTCP_ESTATE;
    r->busy = true;
    airplay_rtsp_server_stop(r->server);
    r->server = NULL;
    refresh(r, RTCP_OK);
    receiver_vehicle_reset(r->vehicle);
    rtcp_ui_sync_reset_session(r->ui_sync);
    r->busy = false;
    return RTCP_OK;
}

int rtcp_receiver_destroy(rtcp_receiver_t *r)
{
    if (r == NULL)
        return RTCP_OK;
    if (r->busy)
        return RTCP_ESTATE;
    int result = rtcp_receiver_stop(r);
    if (result != RTCP_OK)
        return result;
    receiver_vehicle_destroy(r->vehicle);
    memset(r, 0, sizeof(*r));
    free(r);
    return RTCP_OK;
}

int rtcp_receiver_get_state(const rtcp_receiver_t *r, rtcp_receiver_state_t *state)
{
    if (r == NULL || state == NULL)
        return RTCP_EINVAL;
    *state = r->state;
    return RTCP_OK;
}

uint16_t rtcp_receiver_port(const rtcp_receiver_t *r)
{
    return r == NULL ? 0 : airplay_rtsp_server_get_port(r->server);
}

bool rtcp_receiver_send_hid_report(rtcp_receiver_t *r, uint32_t hid_uid,
                                   const uint8_t *report, size_t length)
{
    return r != NULL && airplay_rtsp_server_send_hid_report(r->server,
                                                            hid_uid, report, length);
}

bool rtcp_receiver_request_siri(rtcp_receiver_t *r)
{
    return r != NULL && airplay_rtsp_server_request_siri(r->server);
}

bool rtcp_receiver_request_screen_keyframe(rtcp_receiver_t *receiver)
{
    return receiver && airplay_rtsp_server_request_screen_keyframe(receiver->server);
}

bool rtcp_receiver_send_touch(rtcp_receiver_t *receiver, const rtcp_touch_contact_t contacts[RTCP_TOUCH_CONTACTS])
{
    if (!contacts)
        return false;
    uint8_t report[INPUT_BRIDGE_HID_REPORT_BYTES];
    input_bridge_encode_touch(contacts, report);
    return rtcp_receiver_send_hid_report(receiver, INPUT_BRIDGE_HID_UID_TOUCH, report, sizeof(report));
}

bool rtcp_receiver_send_knob(rtcp_receiver_t *receiver, const rtcp_knob_report_t *state)
{
    if (!state || state->x == INT8_MIN || state->y == INT8_MIN || state->wheel == INT8_MIN)
        return false;
    uint8_t report[] = {
        (uint8_t)(state->select | (state->home << 1) | (state->back << 2)),
        (uint8_t)state->x,
        (uint8_t)state->y,
        (uint8_t)state->wheel,
    };
    return rtcp_receiver_send_hid_report(receiver, INPUT_BRIDGE_HID_UID_KNOB, report, sizeof(report));
}

bool rtcp_receiver_send_media_button(rtcp_receiver_t *receiver, rtcp_media_button_t button)
{
    if ((unsigned)button > RTCP_MEDIA_PREVIOUS)
        return false;
    uint8_t report = (uint8_t)button;
    return rtcp_receiver_send_hid_report(receiver, INPUT_BRIDGE_HID_UID_MEDIA, &report, sizeof(report));
}

bool rtcp_receiver_send_iap(rtcp_receiver_t *r, const uint8_t *bytes, size_t length)
{
    return r != NULL && airplay_rtsp_server_send_iap(r->server, bytes, length);
}

int rtcp_receiver_set_projection_enabled(rtcp_receiver_t *r, bool enabled)
{
    return r ? airplay_rtsp_server_set_projection_enabled(r->server, enabled) : RTCP_EINVAL;
}

rtcp_projection_owner_t rtcp_receiver_get_projection_owner(const rtcp_receiver_t *r)
{
    switch (r ? airplay_rtsp_server_screen_entity(r->server) : 0U)
    {
    case 1U:
        return RTCP_PROJECTION_PHONE;
    case 2U:
        return RTCP_PROJECTION_HOST;
    default:
        return RTCP_PROJECTION_UNKNOWN;
    }
}

uint32_t rtcp_receiver_get_projection_stream_generation(const rtcp_receiver_t *r)
{
    return r ? airplay_rtsp_server_screen_generation(r->server) : 0U;
}

int rtcp_receiver_set_host_ui_callback(rtcp_receiver_t *r, rtcp_host_ui_cb_t callback, void *context)
{
    if (!r)
        return RTCP_EINVAL;
    r->on_host_ui = callback;
    r->host_ui_context = context;
    return RTCP_OK;
}

int rtcp_receiver_set_appearance(rtcp_receiver_t *r, rtcp_appearance_setting_t setting,
                                 bool ui_night, bool map_night)
{
    if (!r || (unsigned)setting > RTCP_APPEARANCE_DARK)
        return RTCP_EINVAL;
    if (setting != RTCP_APPEARANCE_AUTOMATIC)
        ui_night = map_night = setting == RTCP_APPEARANCE_DARK;
    return airplay_rtsp_server_set_appearance(r->server, setting, ui_night, map_night);
}

int rtcp_receiver_set_volume_db(rtcp_receiver_t *receiver, float volume_db)
{
    if (!receiver || !(volume_db >= -144.0f && volume_db <= 0.0f))
        return RTCP_EINVAL;
    receiver->volume_db = volume_db;
    receiver->has_volume = true;
    return RTCP_OK;
}

int rtcp_receiver_set_now_playing_callback(rtcp_receiver_t *receiver, rtcp_now_playing_cb_t callback, void *context)
{
    if (!receiver)
        return RTCP_EINVAL;
    receiver->on_now_playing = callback;
    receiver->now_playing_context = context;
    return RTCP_OK;
}

int rtcp_receiver_set_now_playing_details_callback(rtcp_receiver_t *receiver,
                                                   rtcp_now_playing_details_cb_t callback, void *context)
{
    if (!receiver)
        return RTCP_EINVAL;
    receiver->on_now_playing_details = callback;
    receiver->now_playing_details_context = context;
    return RTCP_OK;
}

int rtcp_receiver_get_peer(const rtcp_receiver_t *r, rtcp_addr_t *peer)
{
    if (r == NULL || peer == NULL)
        return RTCP_EINVAL;
    memset(peer, 0, sizeof(*peer));
    return airplay_rtsp_server_get_peer(r->server, peer) ? RTCP_OK : RTCP_ENOENT;
}

int rtcp_receiver_get_diagnostics(const rtcp_receiver_t *r,
                                  rtcp_receiver_diagnostics_t *diagnostics)
{
    if (r == NULL || diagnostics == NULL)
        return RTCP_EINVAL;
    memset(diagnostics, 0, sizeof(*diagnostics));
    airplay_rtsp_server_get_screen_stats(r->server, &diagnostics->screen);
    airplay_rtsp_server_get_audio_stats(r->server, &diagnostics->audio);
    airplay_rtsp_server_get_timing_stats(r->server, &diagnostics->timing);
    diagnostics->single_cipher_frame = airplay_rtsp_server_single_cipher_frame(r->server);
    return RTCP_OK;
}

int rtcp_receiver_get_alt_video_status(const rtcp_receiver_t *r, rtcp_alt_video_status_t *out)
{
    if (!r || !out)
        return RTCP_EINVAL;
    airplay_rtsp_server_get_alt_video_status(r->server, out);
    out->configured = r->airplay.experimental_alt_video;
    return RTCP_OK;
}

int rtcp_receiver_set_single_cipher_frame(rtcp_receiver_t *r, bool enabled)
{
    if (r == NULL)
        return RTCP_EINVAL;
    if (r->server == NULL)
        return RTCP_ESTATE;
    airplay_rtsp_server_set_single_cipher_frame(r->server, enabled);
    return RTCP_OK;
}

int rtcp_receiver_send_data_stream(rtcp_receiver_t *r, rtcp_data_stream_kind_t kind,
                                   const uint8_t *body, size_t length)
{
    if (!r)
        return RTCP_EINVAL;
    if (r->vehicle && (kind == RTCP_DATA_STREAM_VEHICLE || kind == RTCP_DATA_STREAM_VEHICLE_HIGH))
        return RTCP_ESTATE; /* Attached processor owns these output queues. */
    if (r->ui_sync && kind == RTCP_DATA_STREAM_UI_SYNC)
        return RTCP_ESTATE;
    return airplay_rtsp_server_send_data_stream(r->server, kind, body, length);
}

int rtcp_receiver_get_data_stream_status(const rtcp_receiver_t *r,
                                         rtcp_data_stream_kind_t kind, rtcp_data_stream_status_t *out)
{
    if (!r || !out || (unsigned)kind >= RTCP_DATA_STREAM_COUNT)
        return RTCP_EINVAL;
    if (r->server)
        return airplay_rtsp_server_get_data_stream_status(r->server, kind, out);
    memset(out, 0, sizeof(*out));
    out->registered = (r->airplay.data_stream.channel_mask & RTCP_DATA_STREAM_MASK(kind)) != 0;
    return RTCP_OK;
}

int rtcp_receiver_get_vehicle_delivery_status(const rtcp_receiver_t *r,
                                              rtcp_vehicle_delivery_status_t *out)
{
    if (!r || !out)
        return RTCP_EINVAL;
    receiver_vehicle_status(r->vehicle, out);
    return RTCP_OK;
}

int rtcp_receiver_get_ui_sync_delivery_status(const rtcp_receiver_t *r,
                                              rtcp_ui_sync_delivery_status_t *out)
{
    if (!r || !out)
        return RTCP_EINVAL;
    *out = r->ui_delivery;
    return RTCP_OK;
}
