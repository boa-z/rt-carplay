/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Single-client RTSP socket lifecycle for the AirPlay session router.  The
 * upstream ESP-IDF task/select loop is replaced by a poll() entry point so the
 * server can share an RT-Thread application thread; no platform API appears
 * here.
 */
#include "airplay_rtsp_server.h"
#include "airplay_iap_tunnel.h"
#include "airplay_data_stream.h"

#include <stdlib.h>
#include <string.h>

#include "airplay_bplist.h"
#include "airplay_control_cipher.h"
#include "airplay_event_lifecycle.h"
#include "airplay_event_port.h"
#include "airplay_pair_verify.h"
#include "airplay_screen_lifecycle.h"
#include "airplay_screen_server.h"
#include "airplay_timing_port.h"
#include "mbedtls/platform_util.h"
#include "rtcp/log.h"
#include "rtcp/rtcp.h"

#define TAG "airplay_rtsp"

#define RTSP_RX_CHUNK_BYTES      1200U
#define RTSP_STREAM_BUFFER_BYTES 6144U
/* A whole response is sealed in <=1024-byte frames, so the encrypted buffer
 * is the plaintext maximum plus one 18-byte overhead per chunk. */
#define RTSP_CIPHER_CHUNKS                               \
    ((AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES +          \
      AIRPLAY_CONTROL_CIPHER_MESSAGE_CHUNK_BYTES - 1U) / \
     AIRPLAY_CONTROL_CIPHER_MESSAGE_CHUNK_BYTES)
#define RTSP_CIPHER_FRAME_BYTES                \
    (AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES + \
     RTSP_CIPHER_CHUNKS * AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES)

typedef struct
{
    uint8_t stream[RTSP_STREAM_BUFFER_BYTES];
    size_t stream_len;
    uint8_t response[AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES];
    uint8_t io[RTSP_CIPHER_FRAME_BYTES];
    uint8_t rx[RTSP_RX_CHUNK_BYTES];
    airplay_rtsp_request_t request;
    airplay_control_cipher_t *cipher;
    /* Diagnostics: one id per accepted connection so the log can line up
     * requests, responses and the close that follows them. */
    unsigned id;
    uint32_t opened_ms;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    unsigned tx_frames;
    unsigned requests;
    unsigned rejected_traces;
} rtsp_connection_t;

typedef struct
{
    airplay_rtsp_server_t *owner;
    rtcp_data_stream_kind_t kind;
    airplay_data_stream_t *stream;
    rtcp_data_stream_status_t last;
} server_data_stream_t;

struct airplay_rtsp_server
{
    airplay_rtsp_server_config_t config;
    const struct rtcp_os *os;
    const struct rtcp_net *net;

    uint8_t info_plist[AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES];
    size_t info_plist_len;

    int listener_fd;
    int client_fd;
    uint16_t port;
    uint8_t family;
    volatile bool single_cipher_frame;
    rtcp_addr_t peer;
    bool stopping;
    unsigned next_connection_id;

    airplay_rtsp_session_t *session;
    rtsp_connection_t connection;

    airplay_iap_tunnel_t *iap_tunnel;
    airplay_event_port_t *event_port;
    airplay_timing_port_t *timing_port;
    airplay_screen_server_t *screen_server;
    airplay_audio_stream_t *audio[AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS];
    bool input_active;
    unsigned input_slot;
    bool event_connected;
    bool event_key_used;
    bool event_failed;
    bool event_stopping;
    bool screen_failed;
    uint32_t screen_generation;
    airplay_screen_server_t *alt_screen_server;
    bool alt_screen_failed;
    rtcp_alt_video_status_t alt_video;
    server_data_stream_t data_stream[RTCP_DATA_STREAM_COUNT];
};

static void server_stop_event(void *context);
static void server_stop_timing(void *context);
static void server_stop_screen(void *context);
static void server_stop_audio_slot(airplay_rtsp_server_t *server, unsigned slot);

static void data_stream_snapshot(const server_data_stream_t *slot, rtcp_data_stream_status_t *out)
{
    *out = slot->last;
    if (slot->stream)
    {
        airplay_data_stream_status_t status;
        (void)airplay_data_stream_status(slot->stream, &status);
        out->state = status.state == AIRPLAY_DATA_STREAM_LISTENING ? RTCP_DATA_STREAM_LISTENING : status.state == AIRPLAY_DATA_STREAM_CONNECTED ? RTCP_DATA_STREAM_CONNECTED
                                                                                                                                                : RTCP_DATA_STREAM_FAILED;
        out->error = status.error;
        out->received = status.received;
        out->delivered = status.delivered;
        out->ignored = status.ignored;
        out->sent = status.sent;
        out->sync_replies = status.sync_replies;
        out->rejected_peers = status.rejected_peers;
        out->queued_packages = status.queued_packages;
        out->queued_bytes = status.queued_bytes;
    }
}

static void retire_data_stream(airplay_rtsp_server_t *server, unsigned kind, int error)
{
    server_data_stream_t *slot = &server->data_stream[kind];
    if (!slot->stream)
        return;
    data_stream_snapshot(slot, &slot->last);
    airplay_data_stream_stop(slot->stream);
    slot->stream = NULL;
    slot->last.state = error ? RTCP_DATA_STREAM_FAILED : RTCP_DATA_STREAM_CLOSED;
    slot->last.error = error;
    slot->last.port = 0;
    slot->last.queued_packages = slot->last.queued_bytes = 0;
    airplay_rtsp_session_data_stream_closed(server->session, (rtcp_data_stream_kind_t)kind);
    if (server->config.data_stream.on_closed)
        server->config.data_stream.on_closed((rtcp_data_stream_kind_t)kind, error,
                                             server->config.data_stream.context);
}

static void server_stop_data_stream(rtcp_data_stream_kind_t kind, void *context)
{
    if ((unsigned)kind < RTCP_DATA_STREAM_COUNT)
        retire_data_stream(context, (unsigned)kind, 0);
}

static bool server_data_stream_body(const uint8_t *body, size_t length, void *context)
{
    server_data_stream_t *slot = context;
    return slot->owner->config.data_stream.on_body(slot->kind, body, length,
                                                   slot->owner->config.data_stream.context);
}

static bool server_start_data_stream(const airplay_rtsp_pairing_t *pairing,
                                     rtcp_data_stream_kind_t kind, uint64_t seed, uint32_t id, uint16_t *port, void *context)
{
    airplay_rtsp_server_t *server = context;
    if ((unsigned)kind >= RTCP_DATA_STREAM_COUNT)
        return false;
    server_data_stream_t *slot = &server->data_stream[kind];
    if (slot->stream || !server->config.data_stream.on_body)
        return false;
    uint8_t read_key[32] = { 0 }, write_key[32] = { 0 };
    bool keys = airplay_rtsp_pairing_derive_output_key(pairing, seed, read_key) == AIRPLAY_PAIR_VERIFY_OK &&
                airplay_rtsp_pairing_derive_input_key(pairing, seed, write_key) == AIRPLAY_PAIR_VERIFY_OK;
    slot->owner = server;
    slot->kind = kind;
    int result = keys ? airplay_data_stream_start(server->net, &server->peer,
                                                  read_key, write_key, server_data_stream_body, slot, &slot->stream)
                      : RTCP_ESTATE;
    mbedtls_platform_zeroize(read_key, sizeof(read_key));
    mbedtls_platform_zeroize(write_key, sizeof(write_key));
    slot->last = (rtcp_data_stream_status_t){ .stream_id = id,
                                              .state = result ? RTCP_DATA_STREAM_FAILED : RTCP_DATA_STREAM_LISTENING,
                                              .error = result };
    if (result)
        return false;
    *port = slot->last.port = airplay_data_stream_port(slot->stream);
    return true;
}

/* ------------------------------------------------------------------ */
/* Media endpoints                                                     */
/* ------------------------------------------------------------------ */

static void server_event_state(bool connected, void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL)
        return;
    server->event_connected = connected;
    if (connected)
    {
        server->event_key_used = true;
    }
    else if (airplay_event_lifecycle_is_unexpected_exit(
                 connected, server->event_stopping, server->stopping))
    {
        /* The event port reports false on every unrequested exit, including
         * listener failures before a peer ever attached. */
        server->event_failed = true;
        RTCP_LOGW(TAG,
                  "event_channel=lost rtsp_session=continuing input=disabled "
                  "key_reuse=blocked");
    }
    if (server->config.on_event_state != NULL)
    {
        server->config.on_event_state(connected,
                                      server->config.event_state_context);
    }
}

static void server_screen_exited(void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL)
        return;
    if (!airplay_screen_lifecycle_is_unexpected_exit(server->stopping))
        return;
    RTCP_LOGW(TAG, "screen_stream=lost rtsp_session=closing key_reuse=blocked");
    server->screen_failed = true;
}

static bool server_start_screen(const airplay_rtsp_pairing_t *pairing,
                                uint64_t stream_connection_id,
                                uint16_t requested_port,
                                uint16_t *bound_port,
                                void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || bound_port == NULL)
        return false;
    if (server->screen_server != NULL)
    {
        *bound_port = airplay_screen_server_get_port(server->screen_server);
        return *bound_port != 0U;
    }

    uint8_t output_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
    if (airplay_rtsp_pairing_derive_screen_output_key(
            pairing, stream_connection_id, output_key) != AIRPLAY_PAIR_VERIFY_OK)
    {
        mbedtls_platform_zeroize(output_key, sizeof(output_key));
        RTCP_LOGE(TAG, "screen_setup=key_derivation_failed connection_id=0x%08x%08x",
                  (unsigned)(stream_connection_id >> 32), (unsigned)(uint32_t)stream_connection_id);
        return false;
    }

    airplay_screen_server_config_t config = { 0 };
    config.requested_port = requested_port;
    config.family = server->family;
    config.max_body_bytes = server->config.screen_max_body_bytes;
    config.on_message = server->config.on_screen_message;
    config.message_context = server->config.screen_message_context;
    config.on_unexpected_exit = server_screen_exited;
    config.exit_context = server;

    int result = airplay_screen_server_start(server->os, server->net, output_key,
                                             &config, &server->screen_server);
    mbedtls_platform_zeroize(output_key, sizeof(output_key));
    if (result != RTCP_OK)
    {
        RTCP_LOGE(TAG, "screen_setup=listener_failed rc=%d", result);
        server->screen_server = NULL;
        return false;
    }
    *bound_port = airplay_screen_server_get_port(server->screen_server);
    if (*bound_port == 0U)
    {
        server_stop_screen(server);
        return false;
    }
    if (++server->screen_generation == 0U)
        ++server->screen_generation;
    RTCP_LOGI(TAG, "screen_setup=ready port=%u connection_id=0x%08x%08x",
              (unsigned)*bound_port, (unsigned)(stream_connection_id >> 32),
              (unsigned)(stream_connection_id & UINT32_MAX));
    return true;
}

static void server_alt_screen_exited(void *context)
{
    airplay_rtsp_server_t *s = context;
    if (s && !s->stopping)
        s->alt_screen_failed = true;
}

static void server_stop_video(uint16_t type, void *context)
{
    airplay_rtsp_server_t *s = context;
    if (type == 110)
    {
        server_stop_screen(s);
        return;
    }
    if (type != 111 || !s->alt_screen_server)
        return;
    airplay_screen_server_get_stats(s->alt_screen_server, &s->alt_video.screen);
    airplay_screen_server_stop(s->alt_screen_server, s->net);
    s->alt_screen_server = NULL;
    s->alt_video.ready = false;
    s->alt_video.port = 0;
    ++s->alt_video.closed;
    s->alt_screen_failed = false;
}

static bool server_start_video(const airplay_rtsp_pairing_t *pairing,
                               uint16_t type, uint64_t id, uint16_t requested, uint16_t *port, void *context)
{
    airplay_rtsp_server_t *s = context;
    if (type == 110)
        return server_start_screen(pairing, id, requested, port, s);
    if (type != 111 || !s->config.experimental_alt_video || !s->connection.cipher ||
        s->alt_screen_server)
        return false;
    uint8_t key[AIRPLAY_SCREEN_KEY_BYTES] = { 0 };
    int result = RTCP_ESTATE;
    if (airplay_rtsp_pairing_derive_screen_output_key(pairing, id, key) == AIRPLAY_PAIR_VERIFY_OK)
    {
        airplay_screen_server_config_t config = {
            .requested_port = requested,
            .family = s->family,
            .max_body_bytes = s->config.screen_max_body_bytes,
            .on_message = s->config.on_alt_screen_message,
            .message_context = s->config.alt_screen_message_context,
            .on_unexpected_exit = server_alt_screen_exited,
            .exit_context = s,
        };
        result = airplay_screen_server_start(s->os, s->net, key, &config, &s->alt_screen_server);
    }
    mbedtls_platform_zeroize(key, sizeof(key));
    s->alt_video.last_error = result;
    if (result)
        return false;
    memset(&s->alt_video.screen, 0, sizeof(s->alt_video.screen));
    s->alt_video.port = *port = airplay_screen_server_get_port(s->alt_screen_server);
    s->alt_video.ready = true;
    ++s->alt_video.opened;
    s->alt_screen_failed = false;
    return true;
}

static void retire_failed_alt_video(airplay_rtsp_server_t *s)
{
    if (!s->alt_screen_failed)
        return;
    server_stop_video(111, s);
    s->alt_video.last_error = RTCP_EIO;
    airplay_rtsp_session_video_closed(s->session, 111);
}

static bool server_start_event(const airplay_rtsp_pairing_t *pairing,
                               uint16_t *bound_port,
                               void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || bound_port == NULL)
        return false;

    bool listener_active = server->event_port != NULL;
    if (!airplay_event_lifecycle_may_start(listener_active,
                                           server->event_key_used,
                                           server->event_failed))
    {
        RTCP_LOGW(TAG, "event_setup=refused key_already_used=%u failed=%u",
                  server->event_key_used ? 1U : 0U,
                  server->event_failed ? 1U : 0U);
        return false;
    }
    if (listener_active)
    {
        *bound_port = airplay_event_port_get_port(server->event_port);
        return *bound_port != 0U;
    }

    airplay_pair_verify_event_keys_t keys;
    airplay_pair_verify_result_t key_result =
        airplay_rtsp_pairing_get_event_keys(pairing, &keys);
    if (key_result != AIRPLAY_PAIR_VERIFY_OK)
    {
        mbedtls_platform_zeroize(&keys, sizeof(keys));
        RTCP_LOGW(TAG, "event_setup=pair_verify_not_ready result=%d",
                  (int)key_result);
        return false;
    }

    /* Event AEAD counters restart at zero on attach, so a stopped listener's
     * Pair-Verify keys must not be attached a second time. */
    airplay_event_port_config_t config = { 0 };
    config.keys = keys;
    config.stream_id = 0U;
    config.initial_cseq = 0U;
    config.family = server->family;
    config.on_state = server_event_state;
    config.context = server;
    int result = airplay_event_port_start(server->os, server->net, &config,
                                          &server->event_port);
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    mbedtls_platform_zeroize(&config, sizeof(config));
    if (result != RTCP_OK)
    {
        server->event_port = NULL;
        RTCP_LOGE(TAG, "event_setup=listener_failed rc=%d", result);
        return false;
    }
    *bound_port = airplay_event_port_get_port(server->event_port);
    if (*bound_port == 0U)
    {
        server_stop_event(server);
        return false;
    }
    RTCP_LOGI(TAG, "event_setup=ready port=%u", (unsigned)*bound_port);
    return true;
}

static bool server_start_timing(uint16_t peer_port, uint16_t *bound_port,
                                void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || bound_port == NULL)
        return false;
    if (server->timing_port != NULL)
    {
        *bound_port = airplay_timing_port_get_port(server->timing_port);
        return *bound_port != 0U;
    }
    if (server->peer.family != RTCP_AF_INET &&
        server->peer.family != RTCP_AF_INET6)
    {
        RTCP_LOGW(TAG, "timing_setup=peer_address_unavailable");
        return false;
    }

    airplay_timing_port_config_t config = { 0 };
    config.peer = server->peer;
    config.peer.port = peer_port;
    if (airplay_timing_port_start(server->os, server->net, &config,
                                  &server->timing_port) != RTCP_OK)
    {
        server->timing_port = NULL;
        RTCP_LOGE(TAG, "timing_setup=listener_failed");
        return false;
    }
    *bound_port = airplay_timing_port_get_port(server->timing_port);
    if (*bound_port == 0U)
    {
        server_stop_timing(server);
        return false;
    }
    RTCP_LOGI(TAG, "timing_setup=ready port=%u peer_port=%u",
              (unsigned)*bound_port, (unsigned)peer_port);
    return true;
}

static void server_stop_event(void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || server->event_port == NULL)
        return;
    airplay_event_port_t *event_port = server->event_port;
    server->event_port = NULL;
    server->event_connected = false;
    server->event_stopping = true;
    airplay_event_port_stop(event_port, server->net);
    server->event_stopping = false;
}

static void server_stop_timing(void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || server->timing_port == NULL)
        return;
    airplay_timing_port_t *timing_port = server->timing_port;
    server->timing_port = NULL;
    airplay_timing_port_stop(timing_port, server->net);
}

static void server_stop_screen(void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || server->screen_server == NULL)
        return;
    airplay_screen_server_t *screen_server = server->screen_server;
    server->screen_server = NULL;
    airplay_screen_server_stop(screen_server, server->net);
}

/* stream types 100/101/102 map to slots 0/1/2 */
static int server_audio_pcm(uint16_t stream_type, const uint8_t *data,
                            size_t len, const airplay_audio_format_t *format,
                            uint32_t rtp_timestamp, uint64_t arrival_us,
                            void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || server->config.on_audio_pcm == NULL)
        return RTCP_OK;
    return server->config.on_audio_pcm(stream_type, data, len, format,
                                       rtp_timestamp, arrival_us,
                                       server->config.audio_pcm_context);
}

static bool server_start_audio(const airplay_rtsp_pairing_t *pairing, uint16_t stream_type,
                               const airplay_audio_options_t *options,
                               uint16_t *data_port, uint16_t *control_port, void *context)
{
    uint64_t stream_connection_id = options->connection_id;
    uint64_t audio_format_bits = options->format_bits;
    airplay_rtsp_server_t *server = context;
    if (server == NULL || data_port == NULL || control_port == NULL)
        return false;
    int slot = airplay_audio_slot_for(stream_type, options->audio_type);
    if (slot < 0)
        return false;
    if (server->audio[slot] != NULL)
    {
        *data_port = airplay_audio_stream_get_data_port(server->audio[slot]);
        *control_port = airplay_audio_stream_get_control_port(server->audio[slot]);
        return *data_port != 0U && *control_port != 0U;
    }
    if (server->config.on_audio_pcm == NULL)
    {
        RTCP_LOGW(TAG, "audio_setup=rejected reason=no_pcm_sink type=%u",
                  (unsigned)stream_type);
        return false;
    }

    uint8_t output_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
    if (airplay_rtsp_pairing_derive_output_key(pairing, stream_connection_id,
                                               output_key) !=
        AIRPLAY_PAIR_VERIFY_OK)
    {
        mbedtls_platform_zeroize(output_key, sizeof(output_key));
        RTCP_LOGE(TAG, "audio_setup=key_derivation_failed connection_id=0x%08x%08x",
                  (unsigned)(stream_connection_id >> 32), (unsigned)(uint32_t)stream_connection_id);
        return false;
    }

    airplay_audio_stream_config_t config;
    memset(&config, 0, sizeof(config));
    memcpy(config.output_key, output_key, sizeof(config.output_key));
    config.stream_type = stream_type;
    config.audio_format_bits = audio_format_bits;
    config.family = server->family;
    config.stream_slot = (uint8_t)slot;
    config.stream_slot_valid = true;
    config.on_pcm = server_audio_pcm;
    config.context = server;
    int result = airplay_audio_stream_start(server->os, server->net, &config,
                                            &server->audio[slot]);
    mbedtls_platform_zeroize(output_key, sizeof(output_key));
    mbedtls_platform_zeroize(&config, sizeof(config));
    if (result != RTCP_OK)
    {
        RTCP_LOGE(TAG, "audio_setup=listener_failed type=%u rc=%d",
                  (unsigned)stream_type, result);
        server->audio[slot] = NULL;
        return false;
    }
    *data_port = airplay_audio_stream_get_data_port(server->audio[slot]);
    *control_port = airplay_audio_stream_get_control_port(server->audio[slot]);
    if (*data_port == 0U || *control_port == 0U)
    {
        server_stop_audio_slot(server, (unsigned)slot);
        return false;
    }
    RTCP_LOGI(TAG, "audio_setup=ready type=%u data_port=%u control_port=%u "
                   "connection_id=0x%08x%08x",
              (unsigned)stream_type, (unsigned)*data_port,
              (unsigned)*control_port,
              (unsigned)(stream_connection_id >> 32), (unsigned)(uint32_t)stream_connection_id);
    return true;
}

static bool server_configure_audio(const airplay_rtsp_pairing_t *pairing, uint16_t type,
                                   const airplay_audio_options_t *options, void *context)
{
    airplay_rtsp_server_t *s = context;
    if (!options->input)
        return true;
    /* A rejected competing SETUP must leave the live microphone untouched. */
    if (!s->config.on_audio_input_start || type != 100 || s->input_active)
        return false;
    int slot = airplay_audio_slot_for(type, options->audio_type);
    if (slot < 0)
        return false;
    airplay_audio_input_config_t input = { 0 };
    if (!rtcp_audio_format_parse(options->format_bits, &input.sender.format) ||
        input.sender.format.channels != 1 || input.sender.format.codec == RTCP_AUDIO_AAC_LC)
        return false;
    input.sender.frames_per_packet = options->frames_per_packet;
    input.sender.payload_type = (uint8_t)type;
    input.peer = s->peer;
    input.peer.port = options->input_port;
    input.net = s->net;
    input.telephony = strcmp(options->audio_type, "telephony") == 0;
    if (airplay_rtsp_pairing_derive_input_key(pairing, options->connection_id, input.sender.input_key) != AIRPLAY_PAIR_VERIFY_OK)
        return false;
    bool ok = s->config.on_audio_input_start(&input, s->config.audio_pcm_context);
    mbedtls_platform_zeroize(&input, sizeof(input));
    if (ok)
    {
        s->input_active = true;
        s->input_slot = (unsigned)slot;
    }
    return ok;
}

static void server_stop_audio_slot(airplay_rtsp_server_t *server, unsigned slot)
{
    if (!server || slot >= AIRPLAY_AUDIO_MAX_STREAMS || !server->audio[slot])
        return;
    airplay_audio_stream_t *stream = server->audio[slot];
    server->audio[slot] = NULL;
    uint16_t type = airplay_audio_stream_get_stream_type(stream);
    if (server->input_active && server->input_slot == slot)
    {
        server->input_active = false;
        if (server->config.on_audio_input_stop)
            server->config.on_audio_input_stop(server->config.audio_pcm_context);
    }
    airplay_audio_stream_stop(stream, server->net);
    if (server->config.on_audio_stop_ex)
        server->config.on_audio_stop_ex(slot, type, server->config.audio_pcm_context);
    else if (server->config.on_audio_stop)
        server->config.on_audio_stop(type, server->config.audio_pcm_context);
}
static void server_stop_audio(uint16_t type, const airplay_audio_options_t *options, void *context)
{
    int slot = airplay_audio_slot_for(type, options->audio_type);
    if (slot >= 0)
        server_stop_audio_slot(context, (unsigned)slot);
}

static bool server_disable_bluetooth(const char *device, void *context)
{
    airplay_rtsp_server_t *s = context;
    return s->config.disable_bluetooth && s->config.disable_bluetooth(device, s->config.iap_context);
}
static void server_audio_modes(const airplay_audio_modes_t *modes, void *context)
{
    airplay_rtsp_server_t *s = context;
    if (s->config.on_audio_modes)
        s->config.on_audio_modes(modes, s->config.audio_pcm_context);
}
static bool server_audio_duck(bool ducked, float db, uint32_t ms, void *context)
{
    airplay_rtsp_server_t *s = context;
    return s->config.on_audio_duck && s->config.on_audio_duck(ducked, db, ms, s->config.audio_pcm_context);
}
static void server_volume(float volume_db, void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || server->config.on_volume == NULL)
        return;
    server->config.on_volume(volume_db, server->config.volume_context);
}

/* ------------------------------------------------------------------ */
/* RTSP connection                                                     */
/* ------------------------------------------------------------------ */

static bool socket_send_all(const struct rtcp_net *net, int fd,
                            const uint8_t *bytes, size_t len)
{
    size_t offset = 0U;
    while (offset < len)
    {
        int sent = net->send(fd, bytes + offset, len - offset);
        if (sent <= 0)
            return false;
        offset += (size_t)sent;
    }
    return true;
}

static uint32_t server_now_ms(const airplay_rtsp_server_t *server)
{
    return server->os != NULL && server->os->now_ms != NULL
               ? (uint32_t)server->os->now_ms()
               : 0U;
}

static unsigned response_status(const uint8_t *response, size_t length)
{
    static const char prefix[] = "RTSP/1.0 ";
    if (response == NULL || length < sizeof(prefix) + 3U ||
        memcmp(response, prefix, sizeof(prefix) - 1U) != 0)
    {
        return 0U;
    }
    const uint8_t *status = response + sizeof(prefix) - 1U;
    if (status[0] < '0' || status[0] > '9' || status[1] < '0' ||
        status[1] > '9' || status[2] < '0' || status[2] > '9')
    {
        return 0U;
    }
    return (unsigned)(status[0] - '0') * 100U +
           (unsigned)(status[1] - '0') * 10U + (unsigned)(status[2] - '0');
}

static void consume_stream(rtsp_connection_t *connection, size_t count)
{
    if (count < connection->stream_len)
    {
        memmove(connection->stream, connection->stream + count,
                connection->stream_len - count);
    }
    connection->stream_len -= count;
}

static bool append_stream(rtsp_connection_t *connection, const uint8_t *bytes,
                          size_t len)
{
    if (len > sizeof(connection->stream) - connection->stream_len)
        return false;
    if (len != 0U)
    {
        memcpy(connection->stream + connection->stream_len, bytes, len);
        connection->stream_len += len;
    }
    return true;
}

static bool send_response(airplay_rtsp_server_t *server,
                          rtsp_connection_t *connection, size_t response_len,
                          size_t *wire_len, unsigned *frames)
{
    if (wire_len != NULL)
        *wire_len = response_len;
    if (frames != NULL)
        *frames = 0U;
    if (connection->cipher == NULL)
    {
        return socket_send_all(server->net, server->client_fd,
                               connection->response, response_len);
    }
    size_t encrypted_len = 0U;
    airplay_control_cipher_result_t result;
    if (server->single_cipher_frame &&
        response_len <= AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES)
    {
        result = airplay_control_cipher_encrypt_message_single(
            connection->cipher, connection->response, response_len,
            connection->io, sizeof(connection->io), &encrypted_len);
    }
    else
    {
        result = airplay_control_cipher_encrypt_message(
            connection->cipher, connection->response, response_len,
            connection->io, sizeof(connection->io), &encrypted_len);
    }
    if (result != AIRPLAY_CONTROL_CIPHER_OK)
        return false;
    if (wire_len != NULL)
        *wire_len = encrypted_len;
    if (frames != NULL)
    {
        *frames = (unsigned)((response_len +
                              AIRPLAY_CONTROL_CIPHER_MESSAGE_CHUNK_BYTES -
                              1U) /
                             AIRPLAY_CONTROL_CIPHER_MESSAGE_CHUNK_BYTES);
        if (server->single_cipher_frame &&
            response_len <= AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES)
        {
            *frames = 1U;
        }
    }
    bool sent = socket_send_all(server->net, server->client_fd, connection->io,
                                encrypted_len);
    mbedtls_platform_zeroize(connection->io, sizeof(connection->io));
    return sent;
}

static bool enable_control_cipher(airplay_rtsp_server_t *server,
                                  rtsp_connection_t *connection)
{
    if (connection->stream_len != 0U)
    {
        RTCP_LOGW(TAG, "rtsp_cipher=transition_with_pending_plaintext");
        return false;
    }
    airplay_pair_verify_control_keys_t keys;
    airplay_pair_verify_result_t key_result =
        airplay_rtsp_session_get_control_keys(server->session, &keys);
    if (key_result != AIRPLAY_PAIR_VERIFY_OK ||
        airplay_control_cipher_create(keys.read_key, keys.write_key,
                                      &connection->cipher) !=
            AIRPLAY_CONTROL_CIPHER_OK)
    {
        mbedtls_platform_zeroize(&keys, sizeof(keys));
        RTCP_LOGE(TAG, "rtsp_cipher=enable_failed result=%d", (int)key_result);
        return false;
    }
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    airplay_rtsp_session_mark_control_cipher_active(server->session);
    RTCP_LOGI(TAG, "rtsp_cipher=enabled id=%u ms=%u", connection->id,
              (unsigned)(server_now_ms(server) - connection->opened_ms));
    return true;
}

static bool process_stream(airplay_rtsp_server_t *server,
                           rtsp_connection_t *connection)
{
    while (connection->stream_len != 0U)
    {
        airplay_rtsp_parse_result_t parse_result = airplay_rtsp_parse_request(
            connection->stream, connection->stream_len, &connection->request);
        if (parse_result == AIRPLAY_RTSP_PARSE_INCOMPLETE)
        {
            return connection->stream_len < sizeof(connection->stream);
        }
        if (parse_result != AIRPLAY_RTSP_PARSE_COMPLETE)
        {
            RTCP_LOGW(TAG, "rtsp_request=rejected id=%u parse=%d buffered=%u",
                      connection->id, (int)parse_result,
                      (unsigned)connection->stream_len);
            return false;
        }

        size_t response_len = 0U;
        bool enable_cipher = false;
        airplay_rtsp_session_result_t result = airplay_rtsp_session_handle(
            server->session, &connection->request, connection->response,
            sizeof(connection->response), &response_len, &enable_cipher);
        size_t wire_len = 0U;
        unsigned frames = 0U;
        if (result != AIRPLAY_RTSP_SESSION_OK || response_len == 0U ||
            !send_response(server, connection, response_len, &wire_len,
                           &frames))
        {
            RTCP_LOGE(TAG, "rtsp_response=failed id=%u result=%d method=%s "
                           "path=%s",
                      connection->id, (int)result, connection->request.method,
                      connection->request.path);
            return false;
        }
        size_t consumed = connection->request.consumed;
        unsigned status = response_status(connection->response, response_len);
        connection->requests++;
        connection->tx_bytes += (uint32_t)wire_len;
        connection->tx_frames += frames;
        /* Bounded metadata only: enough to find a silently rejected route or
         * capability request without dumping plists, identifiers or key data.
         * Keep error visibility after the first 32 handshake requests.
         */
        if (connection->requests <= 32U ||
            (status >= 400U && connection->rejected_traces++ < 8U))
        {
            RTCP_LOGI(TAG, "negotiation id=%u request=%u method=%s path=%s body=%u status=%u encrypted=%u",
                      connection->id, connection->requests,
                      connection->request.method, connection->request.path,
                      (unsigned)connection->request.body_len, status,
                      connection->cipher != NULL ? 1U : 0U);
        }
        consume_stream(connection, consumed);
        RTCP_LOGD(TAG, "rtsp_request=handled id=%u ms=%u method=%s path=%s "
                       "cseq=%s bytes=%u",
                  connection->id,
                  (unsigned)(server_now_ms(server) - connection->opened_ms),
                  connection->request.method, connection->request.path,
                  connection->request.cseq, (unsigned)consumed);
        RTCP_LOGD(TAG, "rtsp_response id=%u cseq=%s status=%u plain=%u "
                       "wire=%u frames=%u",
                  connection->id, connection->request.cseq, status,
                  (unsigned)response_len, (unsigned)wire_len, frames);
        memset(&connection->request, 0, sizeof(connection->request));

        if (enable_cipher && !enable_control_cipher(server, connection))
        {
            return false;
        }
    }
    return true;
}

static bool process_received_bytes(airplay_rtsp_server_t *server,
                                   rtsp_connection_t *connection,
                                   const uint8_t *bytes, size_t length)
{
    if (connection->cipher == NULL)
    {
        return append_stream(connection, bytes, length) &&
               process_stream(server, connection);
    }

    size_t offset = 0U;
    bool drain_cipher = true;
    while (offset < length || drain_cipher)
    {
        size_t consumed = 0U;
        size_t plaintext_len = 0U;
        const uint8_t *input = offset < length ? bytes + offset : NULL;
        size_t input_len = offset < length ? length - offset : 0U;
        airplay_control_cipher_result_t result = airplay_control_cipher_decrypt(
            connection->cipher, input, input_len, &consumed,
            connection->response, sizeof(connection->response),
            &plaintext_len);
        offset += consumed;
        if (plaintext_len != 0U &&
            (!append_stream(connection, connection->response, plaintext_len) ||
             !process_stream(server, connection)))
        {
            mbedtls_platform_zeroize(connection->response,
                                     sizeof(connection->response));
            return false;
        }
        mbedtls_platform_zeroize(connection->response,
                                 sizeof(connection->response));
        if (result == AIRPLAY_CONTROL_CIPHER_OUTPUT_TOO_SMALL)
        {
            drain_cipher = true;
            continue;
        }
        if (result == AIRPLAY_CONTROL_CIPHER_OK ||
            result == AIRPLAY_CONTROL_CIPHER_INCOMPLETE)
        {
            if (offset < length)
            {
                if (consumed == 0U)
                    return false;
                drain_cipher = false;
                continue;
            }
            break;
        }
        RTCP_LOGW(TAG, "rtsp_cipher=decrypt_failed result=%d", (int)result);
        return false;
    }
    return true;
}

static void server_teardown_connection(airplay_rtsp_server_t *server,
                                       const char *reason)
{
    if (server->client_fd < 0 && server->session == NULL)
        return;
    RTCP_LOGI(TAG, "rtsp_client=disconnected id=%u reason=%s lifetime_ms=%u",
              server->connection.id, reason,
              (unsigned)(server_now_ms(server) -
                         server->connection.opened_ms));
    RTCP_LOGI(TAG, "rtsp_session_stats id=%u requests=%u rx_bytes=%u "
                   "tx_bytes=%u tx_frames=%u",
              server->connection.id, server->connection.requests,
              (unsigned)server->connection.rx_bytes,
              (unsigned)server->connection.tx_bytes,
              server->connection.tx_frames);
    if (server->session != NULL)
    {
        /* Retire negotiated endpoints before discarding the authenticated
         * connection. The listener remains available for the next peer. */
        airplay_rtsp_session_destroy(server->session);
        server->session = NULL;
    }
    if (server->connection.cipher != NULL)
    {
        airplay_control_cipher_destroy(server->connection.cipher);
        server->connection.cipher = NULL;
    }
    if (server->client_fd >= 0)
    {
        server->net->close(server->client_fd);
        server->client_fd = -1;
    }
    mbedtls_platform_zeroize(server->connection.stream,
                             sizeof(server->connection.stream));
    mbedtls_platform_zeroize(server->connection.response,
                             sizeof(server->connection.response));
    mbedtls_platform_zeroize(server->connection.io,
                             sizeof(server->connection.io));
    mbedtls_platform_zeroize(server->connection.rx,
                             sizeof(server->connection.rx));
    server->connection.stream_len = 0U;
    memset(&server->connection.request, 0, sizeof(server->connection.request));
    server->event_connected = false;
    server->event_key_used = false;
    server->event_failed = false;
    server->screen_failed = false;
    memset(&server->peer, 0, sizeof(server->peer));
}

static bool server_iap_bytes(const uint8_t *p, size_t n, void *context)
{
    airplay_rtsp_server_t *s = context;
    return s->event_connected && s->config.on_iap_bytes &&
           s->config.on_iap_bytes(p, n, s->config.iap_context);
}
static void server_stop_iap(void *context)
{
    airplay_rtsp_server_t *s = context;
    airplay_iap_tunnel_stop(s->iap_tunnel);
    s->iap_tunnel = NULL;
    airplay_rtsp_session_iap_closed(s->session);
    if (s->config.on_iap_stop)
        s->config.on_iap_stop(s->config.iap_context);
}
static bool server_start_iap(const airplay_rtsp_pairing_t *pairing, uint64_t seed,
                             uint16_t *port, void *context)
{
    airplay_rtsp_server_t *s = context;
    uint8_t key[32];
    if (s->iap_tunnel || !s->config.on_iap_bytes ||
        airplay_rtsp_pairing_derive_output_key(pairing, seed, key) != AIRPLAY_PAIR_VERIFY_OK)
        return false;
    int result = airplay_iap_tunnel_start(s->net, &s->peer, key, server_iap_bytes, s, &s->iap_tunnel);
    mbedtls_platform_zeroize(key, sizeof(key));
    if (result != RTCP_OK)
        return false;
    *port = airplay_iap_tunnel_port(s->iap_tunnel);
    return true;
}
bool airplay_rtsp_server_send_iap(airplay_rtsp_server_t *s, const uint8_t *p, size_t n)
{
    return s && !s->stopping && s->event_connected && airplay_event_port_send_iap(s->event_port, p, n);
}

static void server_accept(airplay_rtsp_server_t *server)
{
    rtcp_addr_t peer;
    memset(&peer, 0, sizeof(peer));
    int client_fd = server->net->accept(server->listener_fd, &peer);
    if (client_fd < 0)
    {
        RTCP_LOGW(TAG, "rtsp_accept=failed rc=%d", client_fd);
        return;
    }
    server->client_fd = client_fd;
    server->peer = peer;
    server->connection.stream_len = 0U;
    server->connection.cipher = NULL;
    server->connection.id = ++server->next_connection_id;
    server->connection.opened_ms = server_now_ms(server);
    server->connection.rx_bytes = 0U;
    server->connection.tx_bytes = 0U;
    server->connection.tx_frames = 0U;
    server->connection.requests = 0U;
    for (unsigned k = 0; k < RTCP_DATA_STREAM_COUNT; ++k)
        memset(&server->data_stream[k].last, 0, sizeof(server->data_stream[k].last));

    if (server->config.get_info_plist != NULL)
    {
        const uint8_t *bytes = NULL;
        size_t length = 0U;
        airplay_bplist_doc_t doc;
        airplay_bplist_object_t root;
        airplay_bplist_type_t type;
        if (!server->config.get_info_plist(&bytes, &length, server->config.info_context) ||
            bytes == NULL || length == 0U || length > sizeof(server->info_plist) ||
            airplay_bplist_open(bytes, length, &doc) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_root(&doc, &root) != AIRPLAY_BPLIST_OK ||
            airplay_bplist_get_type(&doc, root, &type, NULL) != AIRPLAY_BPLIST_OK ||
            type != AIRPLAY_BPLIST_DICTIONARY)
        {
            RTCP_LOGE(TAG, "rtsp_session=info_refresh_failed");
            server_teardown_connection(server, "info_refresh_failed");
            return;
        }
        memmove(server->info_plist, bytes, length);
        server->info_plist_len = length;
    }

    airplay_rtsp_session_config_t session_config = { 0 };
    session_config.pairing = server->config.pairing;
    session_config.info_plist = server->info_plist;
    session_config.info_plist_len = server->info_plist_len;
    session_config.screen_requested_port = server->config.screen_port;
    session_config.start_screen = server_start_screen;
    session_config.start_event = server_start_event;
    session_config.start_timing = server_start_timing;
    session_config.start_audio_ex = server_start_audio;
    session_config.configure_audio = server_configure_audio;
    session_config.stop_screen = server_stop_screen;
    if (server->config.experimental_alt_video)
    {
        session_config.start_video = server_start_video;
        session_config.stop_video = server_stop_video;
        session_config.alt_screen_requested_port = server->config.alt_screen_port;
    }
    session_config.stop_event = server_stop_event;
    session_config.stop_timing = server_stop_timing;
    session_config.stop_audio_ex = server_stop_audio;
    session_config.media_context = server;
    session_config.on_volume = server_volume;
    session_config.get_volume = server->config.get_volume;
    session_config.get_volume_context = server->config.get_volume_context;
    session_config.on_now_playing = server->config.on_now_playing;
    session_config.now_playing_context = server->config.now_playing_context;
    session_config.on_now_playing_details = server->config.on_now_playing_details;
    session_config.now_playing_details_context = server->config.now_playing_details_context;
    session_config.on_host_ui = server->config.on_host_ui;
    session_config.host_ui_context = server->config.host_ui_context;
    session_config.volume_context = server;
    session_config.on_audio_modes = server_audio_modes;
    session_config.on_audio_duck = server_audio_duck;
    session_config.command_context = server;
    if (server->config.on_iap_bytes)
    {
        session_config.start_iap = server_start_iap;
        session_config.stop_iap = server_stop_iap;
    }
    session_config.disable_bluetooth = server_disable_bluetooth;
    session_config.data_stream_mask = server->config.data_stream.channel_mask;
    session_config.negotiate_vehicle = server->config.negotiate_vehicle;
    session_config.negotiate_ui_sync = server->config.negotiate_ui_sync;
    session_config.start_data_stream = server_start_data_stream;
    session_config.stop_data_stream = server_stop_data_stream;

    if (airplay_rtsp_session_create_borrowing_info_plist(
            &session_config, &server->session) != AIRPLAY_RTSP_SESSION_OK)
    {
        RTCP_LOGE(TAG, "rtsp_session=create_failed");
        server->session = NULL;
        server_teardown_connection(server, "session_create_failed");
        return;
    }
    RTCP_LOGI(TAG, "rtsp_client=connected id=%u peer=%u.%u.%u.%u port=%u",
              server->connection.id, peer.addr[0], peer.addr[1], peer.addr[2],
              peer.addr[3], (unsigned)peer.port);
}

static void server_read_client(airplay_rtsp_server_t *server)
{
    int received = server->net->recv(server->client_fd, server->connection.rx,
                                     sizeof(server->connection.rx));
    if (received == 0)
    {
        server_teardown_connection(server, "peer_closed");
        return;
    }
    if (received < 0)
    {
        server_teardown_connection(server, "receive_failed");
        return;
    }
    server->connection.rx_bytes += (uint32_t)received;
    bool ok = process_received_bytes(server, &server->connection,
                                     server->connection.rx,
                                     (size_t)received);
    mbedtls_platform_zeroize(server->connection.rx,
                             sizeof(server->connection.rx));
    if (!ok)
        server_teardown_connection(server, "protocol_error");
    else if (server->screen_failed)
    {
        server_teardown_connection(server, "screen_stream_lost");
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int airplay_rtsp_server_start(const airplay_rtsp_server_config_t *config,
                              airplay_rtsp_server_t **out_server)
{
    if (out_server == NULL)
        return RTCP_EINVAL;
    *out_server = NULL;
    if (config == NULL || config->os == NULL || config->net == NULL ||
        config->net->listen == NULL || config->net->accept == NULL ||
        config->info_plist == NULL || config->info_plist_len == 0U ||
        config->info_plist_len > AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES ||
        config->on_screen_message == NULL ||
        (config->experimental_alt_video && !config->on_alt_screen_message) ||
        (config->data_stream.channel_mask & ~RTCP_DATA_STREAM_ALL_MASK) ||
        (config->data_stream.channel_mask && !config->data_stream.on_body))
    {
        return RTCP_EINVAL;
    }
    if (config->info_plist_len > sizeof(((airplay_rtsp_server_t *)0)->info_plist))
    {
        return RTCP_EINVAL;
    }

    /* Reject a capability document the session could not serve later. */
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    airplay_bplist_type_t type;
    if (airplay_bplist_open(config->info_plist, config->info_plist_len, &doc) !=
            AIRPLAY_BPLIST_OK ||
        airplay_bplist_root(&doc, &root) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_type(&doc, root, &type, NULL) != AIRPLAY_BPLIST_OK ||
        type != AIRPLAY_BPLIST_DICTIONARY)
    {
        return RTCP_EINVAL;
    }

    uint16_t requested_port = config->port == 0U ? AIRPLAY_RTSP_DEFAULT_PORT
                                                 : config->port;
    uint8_t family = config->family == 0U ? RTCP_AF_INET : config->family;
    if (family != RTCP_AF_INET && family != RTCP_AF_INET6)
    {
        return RTCP_EINVAL;
    }
    int listener_fd = config->net->listen(family, requested_port);
    if (listener_fd < 0)
    {
        RTCP_LOGE(TAG, "rtsp_bind=failed rc=%d port=%u", listener_fd,
                  (unsigned)requested_port);
        return listener_fd;
    }

    airplay_rtsp_server_t *server = calloc(1U, sizeof(*server));
    if (server == NULL)
    {
        config->net->close(listener_fd);
        return RTCP_ENOMEM;
    }
    server->config = *config;
    server->alt_video.configured = config->experimental_alt_video;
    server->os = config->os;
    server->net = config->net;
    server->config.screen_max_body_bytes =
        config->screen_max_body_bytes == 0U
            ? AIRPLAY_RTSP_SERVER_SCREEN_MAX_BODY_BYTES
            : config->screen_max_body_bytes;
    memcpy(server->info_plist, config->info_plist, config->info_plist_len);
    server->info_plist_len = config->info_plist_len;
    server->config.info_plist = server->info_plist;
    server->listener_fd = listener_fd;
    server->client_fd = -1;
    server->family = family;
    server->port = config->net->local_port != NULL
                       ? config->net->local_port(listener_fd)
                       : requested_port;
    if (server->port == 0U)
    {
        config->net->close(listener_fd);
        mbedtls_platform_zeroize(server, sizeof(*server));
        free(server);
        return RTCP_EIO;
    }

    *out_server = server;
    RTCP_LOGI(TAG, "rtsp_listener=ready port=%u", (unsigned)server->port);
    return RTCP_OK;
}

uint16_t airplay_rtsp_server_get_port(const airplay_rtsp_server_t *server)
{
    return server == NULL ? 0U : server->port;
}

bool airplay_rtsp_server_is_connected(const airplay_rtsp_server_t *server)
{
    return server != NULL && server->client_fd >= 0;
}

bool airplay_rtsp_server_is_encrypted(const airplay_rtsp_server_t *server)
{
    return server != NULL && server->connection.cipher != NULL;
}

void airplay_rtsp_server_set_single_cipher_frame(airplay_rtsp_server_t *server,
                                                 bool enabled)
{
    if (server != NULL)
        server->single_cipher_frame = enabled;
}

bool airplay_rtsp_server_single_cipher_frame(const airplay_rtsp_server_t *server)
{
    return server != NULL && server->single_cipher_frame;
}

bool airplay_rtsp_server_get_peer(const airplay_rtsp_server_t *server,
                                  rtcp_addr_t *peer)
{
    if (server == NULL || peer == NULL || server->client_fd < 0 ||
        (server->peer.family != RTCP_AF_INET &&
         server->peer.family != RTCP_AF_INET6))
    {
        return false;
    }
    *peer = server->peer;
    return true;
}

bool airplay_rtsp_server_send_hid_report(airplay_rtsp_server_t *server,
                                         uint32_t hid_uid,
                                         const uint8_t *report,
                                         size_t report_len)
{
    if (server == NULL || server->stopping || !server->event_connected)
    {
        return false;
    }
    return airplay_event_port_send_hid_report(server->event_port, hid_uid,
                                              report, report_len);
}

void airplay_rtsp_server_get_screen_stats(const airplay_rtsp_server_t *server,
                                          airplay_screen_server_stats_t *stats)
{
    airplay_screen_server_get_stats(server != NULL ? server->screen_server : NULL,
                                    stats);
}

void airplay_rtsp_server_get_alt_video_status(const airplay_rtsp_server_t *s, rtcp_alt_video_status_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!s)
        return;
    *out = s->alt_video;
    if (s->alt_screen_server)
        airplay_screen_server_get_stats(s->alt_screen_server, &out->screen);
}

int airplay_rtsp_server_poll(airplay_rtsp_server_t *server, int timeout_ms)
{
    if (server == NULL)
        return RTCP_EINVAL;
    if (server->stopping)
        return RTCP_ESTATE;
    airplay_screen_server_owner_tick(server->screen_server);
    airplay_screen_server_owner_tick(server->alt_screen_server);

    int event_fd = airplay_event_port_get_fd(server->event_port);
    int timing_fd = airplay_timing_port_get_fd(server->timing_port);
    int screen_fd = airplay_screen_server_get_fd(server->screen_server);
    int rtsp_fd = server->client_fd >= 0 ? server->client_fd
                                         : server->listener_fd;

    struct rtcp_pollfd fds[6 + 2 * AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS + RTCP_DATA_STREAM_COUNT];
    int count = 0;
    int rtsp_index = -1;
    int iap_index = -1;
    int event_index = -1;
    int timing_index = -1;
    int screen_index = -1;
    int alt_screen_index = -1;
    int data_index[RTCP_DATA_STREAM_COUNT];
    uint32_t data_id[RTCP_DATA_STREAM_COUNT];
    for (unsigned k = 0; k < RTCP_DATA_STREAM_COUNT; ++k)
    {
        data_index[k] = -1;
        data_id[k] = server->data_stream[k].last.stream_id;
    }
    int audio_data_index[AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS];
    int audio_control_index[AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS];
    for (size_t slot = 0U; slot < AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS; ++slot)
    {
        audio_data_index[slot] = -1;
        audio_control_index[slot] = -1;
    }
    if (rtsp_fd >= 0)
    {
        rtsp_index = count;
        fds[count].fd = rtsp_fd;
        fds[count].events = RTCP_POLLIN;
        fds[count].revents = 0;
        ++count;
    }
    int iap_fd = airplay_iap_tunnel_fd(server->iap_tunnel);
    if (iap_fd >= 0)
    {
        iap_index = count;
        fds[count++] = (struct rtcp_pollfd){ .fd = iap_fd, .events = RTCP_POLLIN };
    }
    if (event_fd >= 0)
    {
        event_index = count;
        fds[count].fd = event_fd;
        fds[count].events = RTCP_POLLIN;
        fds[count].revents = 0;
        ++count;
    }
    if (timing_fd >= 0)
    {
        timing_index = count;
        fds[count].fd = timing_fd;
        fds[count].events = RTCP_POLLIN;
        fds[count].revents = 0;
        ++count;
    }
    if (screen_fd >= 0)
    {
        screen_index = count;
        fds[count].fd = screen_fd;
        fds[count].events = RTCP_POLLIN;
        fds[count].revents = 0;
        ++count;
    }
    int alt_fd = airplay_screen_server_get_fd(server->alt_screen_server);
    if (alt_fd >= 0)
    {
        alt_screen_index = count;
        fds[count++] = (struct rtcp_pollfd){ .fd = alt_fd, .events = RTCP_POLLIN };
    }
    for (size_t slot = 0U; slot < AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS; ++slot)
    {
        airplay_audio_stream_t *stream = server->audio[slot];
        if (stream == NULL)
            continue;
        int data_fd = airplay_audio_stream_get_data_fd(stream);
        int control_fd = airplay_audio_stream_get_control_fd(stream);
        if (data_fd >= 0)
        {
            audio_data_index[slot] = count;
            fds[count].fd = data_fd;
            fds[count].events = RTCP_POLLIN;
            fds[count].revents = 0;
            ++count;
        }
        if (control_fd >= 0)
        {
            audio_control_index[slot] = count;
            fds[count].fd = control_fd;
            fds[count].events = RTCP_POLLIN;
            fds[count].revents = 0;
            ++count;
        }
    }

    for (unsigned k = 0; k < RTCP_DATA_STREAM_COUNT; ++k)
    {
        airplay_data_stream_t *stream = server->data_stream[k].stream;
        int fd = airplay_data_stream_fd(stream);
        if (fd >= 0)
        {
            data_index[k] = count;
            fds[count++] = (struct rtcp_pollfd){ .fd = fd, .events = airplay_data_stream_events(stream) };
        }
    }
    int ready = count == 0 ? 0 : server->net->poll(fds, count, timeout_ms);
    if (ready < 0)
    {
        RTCP_LOGW(TAG, "rtsp_poll=failed rc=%d", ready);
        server_teardown_connection(server, "poll_failed");
        return RTCP_EIO;
    }

    /* Timing requests are due on a wall-clock schedule, not on socket
     * readiness, so every iteration services the endpoint. */
    bool timing_readable =
        timing_index >= 0 && (fds[timing_index].revents & RTCP_POLLIN) != 0;
    airplay_timing_port_poll(server->timing_port, timing_readable);

    if (event_index >= 0)
    {
        if (fds[event_index].revents & RTCP_POLLERR)
            airplay_event_port_poll_error(server->event_port);
        else if (fds[event_index].revents & RTCP_POLLIN)
            airplay_event_port_poll(server->event_port);
    }
    for (size_t slot = 0U; slot < AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS; ++slot)
    {
        if (server->audio[slot] == NULL)
            continue;
        bool data_readable = audio_data_index[slot] >= 0 &&
                             (fds[audio_data_index[slot]].revents & RTCP_POLLIN) != 0;
        bool control_readable = audio_control_index[slot] >= 0 &&
                                (fds[audio_control_index[slot]].revents & RTCP_POLLIN) != 0;
        if (data_readable || control_readable)
        {
            airplay_audio_stream_poll(server->audio[slot], data_readable,
                                      control_readable);
        }
    }

    /* Service deadline-sensitive UDP before potentially expensive video work. */
    if (screen_index >= 0 && (fds[screen_index].revents & RTCP_POLLIN) != 0)
    {
        airplay_screen_server_poll(server->screen_server);
    }

    if (alt_screen_index >= 0)
    {
        if (fds[alt_screen_index].revents & RTCP_POLLERR)
            server->alt_screen_failed = true;
        else if (fds[alt_screen_index].revents & RTCP_POLLIN)
            airplay_screen_server_poll(server->alt_screen_server);
    }
    /* Retire before RTSP can replace the slot. No stale exit may close a new key. */
    retire_failed_alt_video(server);

    if (rtsp_index >= 0 && (fds[rtsp_index].revents != 0))
    {
        if (server->client_fd < 0)
        {
            server_accept(server);
        }
        else
        {
            server_read_client(server);
        }
    }

    if (iap_index >= 0 && fds[iap_index].revents &&
        !airplay_iap_tunnel_poll(server->iap_tunnel))
    {
        RTCP_LOGW(TAG, "iap_tunnel=closed");
        server_stop_iap(server);
    }
    if (server->config.on_iap_tick && server->iap_tunnel &&
        !server->config.on_iap_tick(server->config.iap_context))
    {
        RTCP_LOGW(TAG, "iap_tunnel=protocol_failed");
        server_stop_iap(server);
    }
    /* RTSP may retire/reopen a stream before this dispatch. Never apply stale
     * readiness to its replacement, even if the allocator/socket reuses it. */
    for (unsigned k = 0; k < RTCP_DATA_STREAM_COUNT; ++k)
    {
        int index = data_index[k];
        server_data_stream_t *slot = &server->data_stream[k];
        if (index < 0 || !fds[index].revents || data_id[k] != slot->last.stream_id ||
            fds[index].fd != airplay_data_stream_fd(slot->stream))
            continue;
        int result = airplay_data_stream_poll(slot->stream, fds[index].revents);
        if (result)
            retire_data_stream(server, k, result);
    }
    if (server->screen_failed)
    {
        server_teardown_connection(server, "screen_stream_lost");
    }
    return RTCP_OK;
}

void airplay_rtsp_server_stop(airplay_rtsp_server_t *server)
{
    if (server == NULL)
        return;
    server->stopping = true;
    server_teardown_connection(server, "server_stop");
    for (size_t slot = 0U; slot < AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS; ++slot)
    {
        if (server->audio[slot] != NULL)
        {
            server_stop_audio_slot(server, (unsigned)slot);
        }
    }
    if (server->listener_fd >= 0)
    {
        server->net->close(server->listener_fd);
        server->listener_fd = -1;
    }
    mbedtls_platform_zeroize(server->info_plist, sizeof(server->info_plist));
    memset(&server->config, 0, sizeof(server->config));
    mbedtls_platform_zeroize(server, sizeof(*server));
    free(server);
}

bool airplay_rtsp_server_request_siri(airplay_rtsp_server_t *s)
{
    return s && !s->stopping && s->event_connected && airplay_event_port_request_siri(s->event_port);
}

bool airplay_rtsp_server_request_screen_keyframe(airplay_rtsp_server_t *server)
{
    return airplay_rtsp_server_screen_generation(server) != 0 && server->event_connected &&
           airplay_event_port_request_screen_keyframe(server->event_port);
}

int airplay_rtsp_server_set_projection_enabled(airplay_rtsp_server_t *s, bool enabled)
{
    if (!s || s->stopping || !s->event_connected ||
        !airplay_rtsp_session_is_established(s->session))
        return RTCP_ESTATE;
    return airplay_event_port_set_projection_enabled(s->event_port, enabled) ? RTCP_OK : RTCP_EIO;
}

uint32_t airplay_rtsp_server_screen_entity(const airplay_rtsp_server_t *s)
{
    return s && !s->stopping && airplay_rtsp_session_is_established(s->session)
               ? airplay_rtsp_session_screen_entity(s->session)
               : 0U;
}

uint32_t airplay_rtsp_server_screen_generation(const airplay_rtsp_server_t *s)
{
    return s && !s->stopping && !s->screen_failed && s->screen_server
               ? s->screen_generation
               : 0U;
}

int airplay_rtsp_server_set_appearance(airplay_rtsp_server_t *s, unsigned setting,
                                       bool ui_night, bool map_night)
{
    if (!s || s->stopping || !s->event_connected)
        return RTCP_ESTATE;

    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root, displays, main_display, uuid;
    char id[37];
    size_t length;
    if (airplay_bplist_open(s->info_plist, s->info_plist_len, &doc) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_root(&doc, &root) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_dict_get_unique(&doc, root, "displays", &displays) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_array_get(&doc, displays, 0, &main_display) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_dict_get_unique(&doc, main_display, "uuid", &uuid) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_string(&doc, uuid, id, sizeof(id), &length) != AIRPLAY_BPLIST_OK ||
        length != sizeof(id) - 1 || strlen(id) != length)
        return RTCP_ENOTSUP;

    return airplay_event_port_set_appearance(s->event_port, id, setting, ui_night, map_night)
               ? RTCP_OK
               : RTCP_EIO;
}

void airplay_rtsp_server_get_audio_stats(const airplay_rtsp_server_t *server, airplay_rtsp_audio_stats_t *stats)
{
    if (!stats)
        return;
    memset(stats, 0, sizeof(*stats));
    if (!server)
        return;
    for (unsigned i = 0; i < AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS; ++i)
    {
        const airplay_audio_stream_t *stream = server->audio[i];
        if (!stream)
            continue;
        rtcp_audio_stats_t lane;
        airplay_audio_stream_get_stats(stream, &lane);
        ++stats->active_streams;
#define ADD(field) stats->field += lane.field
        ADD(datagrams);
        ADD(auth_failures);
        ADD(malformed);
        ADD(replay_drops);
        ADD(delivered);
        ADD(sink_rejected);
        ADD(sequence_missing);
        ADD(sequence_reordered);
#undef ADD
        if (lane.poll_gap_max_us > stats->poll_gap_max_us)
            stats->poll_gap_max_us = lane.poll_gap_max_us;
    }
}

void airplay_rtsp_server_get_timing_stats(const airplay_rtsp_server_t *server,
                                          airplay_timing_stats_t *stats)
{
    airplay_timing_port_get_stats(server != NULL ? server->timing_port : NULL, stats);
}

void airplay_rtsp_server_get_receiver_state(const airplay_rtsp_server_t *s,
                                            rtcp_receiver_state_t *state)
{
    if (state == NULL)
        return;
    state->transport_connected = airplay_rtsp_server_is_connected(s);
    state->authenticated = airplay_rtsp_server_is_encrypted(s);
    state->session_established = s != NULL && airplay_rtsp_session_is_established(s->session);
    state->video_ready = s != NULL && s->screen_server != NULL;
    state->audio_ready = false;
    if (s != NULL)
    {
        for (unsigned i = 0; i < AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS; ++i)
            if (s->audio[i] != NULL)
                state->audio_ready = true;
    }
    state->input_ready = s != NULL && s->event_connected;
}

int airplay_rtsp_server_send_data_stream(airplay_rtsp_server_t *s, rtcp_data_stream_kind_t kind,
                                         const uint8_t *body, size_t length)
{
    if ((unsigned)kind >= RTCP_DATA_STREAM_COUNT || (!body && length) ||
        length > AIRPLAY_DATA_STREAM_MAX_BODY)
        return RTCP_EINVAL;
    if (!s || s->stopping || !s->data_stream[kind].stream)
        return RTCP_ESTATE;
    return airplay_data_stream_send(s->data_stream[kind].stream, body, length);
}

int airplay_rtsp_server_get_data_stream_status(const airplay_rtsp_server_t *s,
                                               rtcp_data_stream_kind_t kind, rtcp_data_stream_status_t *out)
{
    if (!s || !out || (unsigned)kind >= RTCP_DATA_STREAM_COUNT)
        return RTCP_EINVAL;
    data_stream_snapshot(&s->data_stream[kind], out);
    out->registered = (s->config.data_stream.channel_mask & RTCP_DATA_STREAM_MASK(kind)) != 0;
    out->offered = airplay_rtsp_session_data_stream_offered(s->session, kind);
    out->enabled = airplay_rtsp_session_data_stream_enabled(s->session, kind);
    return RTCP_OK;
}
