/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtcp/receiver.h"
#include "airplay_rtsp_server.h"
#include "rtcp/receiver_compat.h"
#include <stdlib.h>
#include <string.h>

struct rtcp_receiver {
    airplay_rtsp_server_config_t airplay;
    uint8_t info[AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES];
    airplay_rtsp_server_t *server;
    rtcp_receiver_event_cb_t on_state;
    void *context;
    rtcp_receiver_state_t state;
    bool busy;
};

/* An intentional Null Sink lets a headless host exercise the full protocol. */
static int discard_screen(airplay_screen_message_type_t type, uint8_t *bytes,
                           size_t length, size_t prefix, uint64_t sender_ns,
                           uint64_t arrival_us, void *context)
{
    (void)type; (void)bytes; (void)length; (void)prefix;
    (void)sender_ns; (void)arrival_us; (void)context;
    return RTCP_OK;
}

static void refresh(rtcp_receiver_t *r, int error)
{
    rtcp_receiver_state_t next = {0};
    next.generation = r->state.generation;
    next.last_error = error;
    next.running = r->server != NULL;
    airplay_rtsp_server_get_receiver_state(r->server, &next);
    bool changed = next.running != r->state.running ||
        next.transport_connected != r->state.transport_connected ||
        next.authenticated != r->state.authenticated ||
        next.session_established != r->state.session_established ||
        next.video_ready != r->state.video_ready ||
        next.audio_ready != r->state.audio_ready ||
        next.input_ready != r->state.input_ready ||
        next.last_error != r->state.last_error;
    r->state = next;
    if (changed && r->on_state != NULL) r->on_state(&r->state, r->context);
}

int rtcp_receiver_config_from_airplay(const struct airplay_rtsp_server_config *a,
                                      rtcp_receiver_config_t *config)
{
    if (a == NULL || config == NULL) return RTCP_EINVAL;
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
    return RTCP_OK;
}

int rtcp_receiver_create(const rtcp_receiver_config_t *config,
                          rtcp_receiver_t **out)
{
    if (out == NULL) return RTCP_EINVAL;
    *out = NULL;
    if (config == NULL) return RTCP_EINVAL;
    const rtcp_platform_t *platform = &config->platform;
    if (platform->os == NULL || platform->net == NULL ||
        platform->info_plist == NULL || platform->info_plist_len == 0 ||
        platform->info_plist_len > AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES ||
        (config->family != 0 && config->family != RTCP_AF_INET &&
         config->family != RTCP_AF_INET6) ||
        (config->data_stream.channel_mask & ~RTCP_DATA_STREAM_ALL_MASK) ||
        (config->data_stream.channel_mask && !config->data_stream.on_body))
        return RTCP_EINVAL;
    rtcp_receiver_t *r = calloc(1, sizeof(*r));
    if (r == NULL) return RTCP_ENOMEM;
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
        .on_event_state = config->on_event_state,
        .event_state_context = config->event_state_context,
        .on_iap_bytes = config->iap2.on_bytes,
        .on_iap_tick = config->iap2.on_tick,
        .on_iap_stop = config->iap2.on_stop,
        .disable_bluetooth = config->iap2.disable_bluetooth,
        .iap_context = config->iap2.context,
        .data_stream = config->data_stream,
    };
    if (r->airplay.on_screen_message == NULL)
        r->airplay.on_screen_message = discard_screen;
    memcpy(r->info, platform->info_plist, platform->info_plist_len);
    r->airplay.info_plist = r->info;
    r->on_state = config->on_state;
    r->context = config->context;
    *out = r;
    return RTCP_OK;
}

int rtcp_receiver_start(rtcp_receiver_t *r)
{
    if (r == NULL) return RTCP_EINVAL;
    if (r->busy) return RTCP_ESTATE;
    if (r->server != NULL) return RTCP_OK;
    r->busy = true;
    int result = airplay_rtsp_server_start(&r->airplay, &r->server);
    if (result == RTCP_OK) ++r->state.generation;
    refresh(r, result);
    r->busy = false;
    return result;
}

int rtcp_receiver_poll(rtcp_receiver_t *r, int timeout_ms)
{
    if (r == NULL || timeout_ms < 0) return RTCP_EINVAL;
    if (r->busy || r->server == NULL) return RTCP_ESTATE;
    r->busy = true;
    int result = airplay_rtsp_server_poll(r->server, timeout_ms);
    refresh(r, result);
    r->busy = false;
    return result;
}

int rtcp_receiver_stop(rtcp_receiver_t *r)
{
    if (r == NULL) return RTCP_EINVAL;
    if (r->busy) return RTCP_ESTATE;
    r->busy = true;
    airplay_rtsp_server_stop(r->server);
    r->server = NULL;
    refresh(r, RTCP_OK);
    r->busy = false;
    return RTCP_OK;
}

int rtcp_receiver_destroy(rtcp_receiver_t *r)
{
    if (r == NULL) return RTCP_OK;
    if (r->busy) return RTCP_ESTATE;
    int result = rtcp_receiver_stop(r);
    if (result != RTCP_OK) return result;
    memset(r, 0, sizeof(*r));
    free(r);
    return RTCP_OK;
}

int rtcp_receiver_get_state(const rtcp_receiver_t *r, rtcp_receiver_state_t *state)
{
    if (r == NULL || state == NULL) return RTCP_EINVAL;
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

bool rtcp_receiver_send_iap(rtcp_receiver_t *r, const uint8_t *bytes, size_t length)
{
    return r != NULL && airplay_rtsp_server_send_iap(r->server, bytes, length);
}

int rtcp_receiver_get_peer(const rtcp_receiver_t *r, rtcp_addr_t *peer)
{
    if (r == NULL || peer == NULL) return RTCP_EINVAL;
    memset(peer, 0, sizeof(*peer));
    return airplay_rtsp_server_get_peer(r->server, peer) ? RTCP_OK : RTCP_ENOENT;
}

int rtcp_receiver_get_diagnostics(const rtcp_receiver_t *r,
                                  rtcp_receiver_diagnostics_t *diagnostics)
{
    if (r == NULL || diagnostics == NULL) return RTCP_EINVAL;
    memset(diagnostics, 0, sizeof(*diagnostics));
    airplay_rtsp_server_get_screen_stats(r->server, &diagnostics->screen);
    airplay_rtsp_server_get_audio_stats(r->server, &diagnostics->audio);
    airplay_rtsp_server_get_timing_stats(r->server, &diagnostics->timing);
    diagnostics->single_cipher_frame = airplay_rtsp_server_single_cipher_frame(r->server);
    return RTCP_OK;
}

int rtcp_receiver_set_single_cipher_frame(rtcp_receiver_t *r, bool enabled)
{
    if (r == NULL) return RTCP_EINVAL;
    if (r->server == NULL) return RTCP_ESTATE;
    airplay_rtsp_server_set_single_cipher_frame(r->server, enabled);
    return RTCP_OK;
}

int rtcp_receiver_send_data_stream(rtcp_receiver_t *r, rtcp_data_stream_kind_t kind,
    const uint8_t *body, size_t length)
{
    if (!r) return RTCP_EINVAL;
    return airplay_rtsp_server_send_data_stream(r->server, kind, body, length);
}

int rtcp_receiver_get_data_stream_status(const rtcp_receiver_t *r,
    rtcp_data_stream_kind_t kind, rtcp_data_stream_status_t *out)
{
    if (!r || !out || (unsigned)kind >= RTCP_DATA_STREAM_COUNT) return RTCP_EINVAL;
    if (r->server) return airplay_rtsp_server_get_data_stream_status(r->server, kind, out);
    memset(out, 0, sizeof(*out));
    out->registered = (r->airplay.data_stream.channel_mask & RTCP_DATA_STREAM_MASK(kind)) != 0;
    return RTCP_OK;
}
