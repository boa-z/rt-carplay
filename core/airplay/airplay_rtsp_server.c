/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Single-client RTSP socket lifecycle for the AirPlay session router.  The
 * upstream ESP-IDF task/select loop is replaced by a poll() entry point so the
 * server can share an RT-Thread application thread; no platform API appears
 * here.
 */
#include "airplay_rtsp_server.h"

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

#define RTSP_RX_CHUNK_BYTES 1200U
#define RTSP_STREAM_BUFFER_BYTES 6144U
/* A whole response is sealed in <=1024-byte frames, so the encrypted buffer
 * is the plaintext maximum plus one 18-byte overhead per chunk. */
#define RTSP_CIPHER_CHUNKS \
    ((AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES + \
      AIRPLAY_CONTROL_CIPHER_MESSAGE_CHUNK_BYTES - 1U) / \
     AIRPLAY_CONTROL_CIPHER_MESSAGE_CHUNK_BYTES)
#define RTSP_CIPHER_FRAME_BYTES                                       \
    (AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES +                        \
     RTSP_CIPHER_CHUNKS * AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES)

typedef struct {
    uint8_t stream[RTSP_STREAM_BUFFER_BYTES];
    size_t stream_len;
    uint8_t response[AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES];
    uint8_t io[RTSP_CIPHER_FRAME_BYTES];
    uint8_t rx[RTSP_RX_CHUNK_BYTES];
    airplay_rtsp_request_t request;
    airplay_control_cipher_t *cipher;
} rtsp_connection_t;

struct airplay_rtsp_server {
    airplay_rtsp_server_config_t config;
    const struct rtcp_os *os;
    const struct rtcp_net *net;

    uint8_t info_plist[AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES];
    size_t info_plist_len;

    int listener_fd;
    int client_fd;
    uint16_t port;
    rtcp_addr_t peer;
    bool stopping;

    airplay_rtsp_session_t *session;
    rtsp_connection_t connection;

    airplay_event_port_t *event_port;
    airplay_timing_port_t *timing_port;
    airplay_screen_server_t *screen_server;
    airplay_audio_stream_t *audio[AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS];
    bool event_connected;
    bool event_key_used;
    bool event_failed;
    bool event_stopping;
    bool screen_failed;
};

static void server_stop_event(void *context);
static void server_stop_timing(void *context);
static void server_stop_screen(void *context);
static void server_stop_audio(uint16_t stream_type, void *context);

/* ------------------------------------------------------------------ */
/* Media endpoints                                                     */
/* ------------------------------------------------------------------ */

static void server_event_state(bool connected, void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL) return;
    server->event_connected = connected;
    if (connected) {
        server->event_key_used = true;
    } else if (airplay_event_lifecycle_is_unexpected_exit(
                   connected, server->event_stopping, server->stopping)) {
        /* The event port reports false on every unrequested exit, including
         * listener failures before a peer ever attached. */
        server->event_failed = true;
        RTCP_LOGW(TAG,
                  "event_channel=lost rtsp_session=continuing input=disabled "
                  "key_reuse=blocked");
    }
    if (server->config.on_event_state != NULL) {
        server->config.on_event_state(connected,
                                      server->config.event_state_context);
    }
}

static void server_screen_exited(void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL) return;
    if (!airplay_screen_lifecycle_is_unexpected_exit(server->stopping)) return;
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
    if (server == NULL || bound_port == NULL) return false;
    if (server->screen_server != NULL) {
        *bound_port = airplay_screen_server_get_port(server->screen_server);
        return *bound_port != 0U;
    }

    uint8_t output_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
    if (airplay_rtsp_pairing_derive_screen_output_key(
            pairing, stream_connection_id, output_key) != AIRPLAY_PAIR_VERIFY_OK) {
        mbedtls_platform_zeroize(output_key, sizeof(output_key));
        RTCP_LOGE(TAG, "screen_setup=key_derivation_failed connection_id=%llu",
                  (unsigned long long)stream_connection_id);
        return false;
    }

    airplay_screen_server_config_t config = {0};
    config.requested_port = requested_port;
    config.max_body_bytes = server->config.screen_max_body_bytes;
    config.on_message = server->config.on_screen_message;
    config.message_context = server->config.screen_message_context;
    config.on_unexpected_exit = server_screen_exited;
    config.exit_context = server;

    int result = airplay_screen_server_start(server->os, server->net, output_key,
                                             &config, &server->screen_server);
    mbedtls_platform_zeroize(output_key, sizeof(output_key));
    if (result != RTCP_OK) {
        RTCP_LOGE(TAG, "screen_setup=listener_failed rc=%d", result);
        server->screen_server = NULL;
        return false;
    }
    *bound_port = airplay_screen_server_get_port(server->screen_server);
    if (*bound_port == 0U) {
        server_stop_screen(server);
        return false;
    }
    RTCP_LOGI(TAG, "screen_setup=ready port=%u connection_id=%llu",
              (unsigned)*bound_port, (unsigned long long)stream_connection_id);
    return true;
}

static bool server_start_event(const airplay_rtsp_pairing_t *pairing,
                               uint16_t *bound_port,
                               void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || bound_port == NULL) return false;

    bool listener_active = server->event_port != NULL;
    if (!airplay_event_lifecycle_may_start(listener_active,
                                           server->event_key_used,
                                           server->event_failed)) {
        RTCP_LOGW(TAG, "event_setup=refused key_already_used=%u failed=%u",
                  server->event_key_used ? 1U : 0U,
                  server->event_failed ? 1U : 0U);
        return false;
    }
    if (listener_active) {
        *bound_port = airplay_event_port_get_port(server->event_port);
        return *bound_port != 0U;
    }

    airplay_pair_verify_event_keys_t keys;
    airplay_pair_verify_result_t key_result =
        airplay_rtsp_pairing_get_event_keys(pairing, &keys);
    if (key_result != AIRPLAY_PAIR_VERIFY_OK) {
        mbedtls_platform_zeroize(&keys, sizeof(keys));
        RTCP_LOGW(TAG, "event_setup=pair_verify_not_ready result=%d",
                  (int)key_result);
        return false;
    }

    /* Event AEAD counters restart at zero on attach, so a stopped listener's
     * Pair-Verify keys must not be attached a second time. */
    airplay_event_port_config_t config = {0};
    config.keys = keys;
    config.stream_id = 0U;
    config.initial_cseq = 0U;
    config.on_state = server_event_state;
    config.context = server;
    int result = airplay_event_port_start(server->os, server->net, &config,
                                          &server->event_port);
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    mbedtls_platform_zeroize(&config, sizeof(config));
    if (result != RTCP_OK) {
        server->event_port = NULL;
        RTCP_LOGE(TAG, "event_setup=listener_failed rc=%d", result);
        return false;
    }
    *bound_port = airplay_event_port_get_port(server->event_port);
    if (*bound_port == 0U) {
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
    if (server == NULL || bound_port == NULL) return false;
    if (server->timing_port != NULL) {
        *bound_port = airplay_timing_port_get_port(server->timing_port);
        return *bound_port != 0U;
    }
    if (server->peer.family != RTCP_AF_INET) {
        RTCP_LOGW(TAG, "timing_setup=peer_address_unavailable");
        return false;
    }

    airplay_timing_port_config_t config = {0};
    config.peer = server->peer;
    config.peer.port = peer_port;
    if (airplay_timing_port_start(server->os, server->net, &config,
                                  &server->timing_port) != RTCP_OK) {
        server->timing_port = NULL;
        RTCP_LOGE(TAG, "timing_setup=listener_failed");
        return false;
    }
    *bound_port = airplay_timing_port_get_port(server->timing_port);
    if (*bound_port == 0U) {
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
    if (server == NULL || server->event_port == NULL) return;
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
    if (server == NULL || server->timing_port == NULL) return;
    airplay_timing_port_t *timing_port = server->timing_port;
    server->timing_port = NULL;
    airplay_timing_port_stop(timing_port, server->net);
}

static void server_stop_screen(void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || server->screen_server == NULL) return;
    airplay_screen_server_t *screen_server = server->screen_server;
    server->screen_server = NULL;
    airplay_screen_server_stop(screen_server, server->net);
}

/* stream types 100/101/102 map to slots 0/1/2 */
static int server_audio_slot(uint16_t stream_type)
{
    switch (stream_type) {
    case AIRPLAY_AUDIO_STREAM_MAIN: return 0;
    case AIRPLAY_AUDIO_STREAM_ALT: return 1;
    case AIRPLAY_AUDIO_STREAM_HIGH: return 2;
    default: return -1;
    }
}

static int server_audio_pcm(uint16_t stream_type, const uint8_t *data,
                            size_t len, const airplay_audio_format_t *format,
                            uint32_t rtp_timestamp, uint64_t arrival_us,
                            void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || server->config.on_audio_pcm == NULL) return RTCP_OK;
    return server->config.on_audio_pcm(stream_type, data, len, format,
                                       rtp_timestamp, arrival_us,
                                       server->config.audio_pcm_context);
}

static bool server_start_audio(const airplay_rtsp_pairing_t *pairing,
                               uint16_t stream_type,
                               uint64_t stream_connection_id,
                               uint64_t audio_format_bits,
                               uint16_t *data_port,
                               uint16_t *control_port,
                               void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || data_port == NULL || control_port == NULL) return false;
    int slot = server_audio_slot(stream_type);
    if (slot < 0) return false;
    if (server->audio[slot] != NULL) {
        *data_port = airplay_audio_stream_get_data_port(server->audio[slot]);
        *control_port = airplay_audio_stream_get_control_port(server->audio[slot]);
        return *data_port != 0U && *control_port != 0U;
    }
    if (server->config.on_audio_pcm == NULL) {
        RTCP_LOGW(TAG, "audio_setup=rejected reason=no_pcm_sink type=%u",
                  (unsigned)stream_type);
        return false;
    }

    uint8_t output_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
    if (airplay_rtsp_pairing_derive_output_key(pairing, stream_connection_id,
                                               output_key) !=
        AIRPLAY_PAIR_VERIFY_OK) {
        mbedtls_platform_zeroize(output_key, sizeof(output_key));
        RTCP_LOGE(TAG, "audio_setup=key_derivation_failed connection_id=%llu",
                  (unsigned long long)stream_connection_id);
        return false;
    }

    airplay_audio_stream_config_t config;
    memset(&config, 0, sizeof(config));
    memcpy(config.output_key, output_key, sizeof(config.output_key));
    config.stream_type = stream_type;
    config.audio_format_bits = audio_format_bits;
    config.on_pcm = server_audio_pcm;
    config.context = server;
    int result = airplay_audio_stream_start(server->os, server->net, &config,
                                            &server->audio[slot]);
    mbedtls_platform_zeroize(output_key, sizeof(output_key));
    mbedtls_platform_zeroize(&config, sizeof(config));
    if (result != RTCP_OK) {
        RTCP_LOGE(TAG, "audio_setup=listener_failed type=%u rc=%d",
                  (unsigned)stream_type, result);
        server->audio[slot] = NULL;
        return false;
    }
    *data_port = airplay_audio_stream_get_data_port(server->audio[slot]);
    *control_port = airplay_audio_stream_get_control_port(server->audio[slot]);
    if (*data_port == 0U || *control_port == 0U) {
        server_stop_audio(stream_type, server);
        return false;
    }
    RTCP_LOGI(TAG, "audio_setup=ready type=%u data_port=%u control_port=%u "
                   "connection_id=%llu",
              (unsigned)stream_type, (unsigned)*data_port,
              (unsigned)*control_port,
              (unsigned long long)stream_connection_id);
    return true;
}

static void server_stop_audio(uint16_t stream_type, void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL) return;
    int slot = server_audio_slot(stream_type);
    if (slot < 0 || server->audio[slot] == NULL) return;
    airplay_audio_stream_t *stream = server->audio[slot];
    server->audio[slot] = NULL;
    airplay_audio_stream_stop(stream, server->net);
}

static void server_volume(float volume_db, void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || server->config.on_volume == NULL) return;
    server->config.on_volume(volume_db, server->config.volume_context);
}

/* ------------------------------------------------------------------ */
/* RTSP connection                                                     */
/* ------------------------------------------------------------------ */

static bool socket_send_all(const struct rtcp_net *net, int fd,
                            const uint8_t *bytes, size_t len)
{
    size_t offset = 0U;
    while (offset < len) {
        int sent = net->send(fd, bytes + offset, len - offset);
        if (sent <= 0) return false;
        offset += (size_t)sent;
    }
    return true;
}

static void consume_stream(rtsp_connection_t *connection, size_t count)
{
    if (count < connection->stream_len) {
        memmove(connection->stream, connection->stream + count,
                connection->stream_len - count);
    }
    connection->stream_len -= count;
}

static bool append_stream(rtsp_connection_t *connection, const uint8_t *bytes,
                          size_t len)
{
    if (len > sizeof(connection->stream) - connection->stream_len) return false;
    if (len != 0U) {
        memcpy(connection->stream + connection->stream_len, bytes, len);
        connection->stream_len += len;
    }
    return true;
}

static bool send_response(airplay_rtsp_server_t *server,
                          rtsp_connection_t *connection, size_t response_len)
{
    if (connection->cipher == NULL) {
        return socket_send_all(server->net, server->client_fd,
                               connection->response, response_len);
    }
    size_t encrypted_len = 0U;
    airplay_control_cipher_result_t result =
        airplay_control_cipher_encrypt_message(
            connection->cipher, connection->response, response_len,
            connection->io, sizeof(connection->io), &encrypted_len);
    if (result != AIRPLAY_CONTROL_CIPHER_OK) return false;
    bool sent = socket_send_all(server->net, server->client_fd, connection->io,
                                encrypted_len);
    mbedtls_platform_zeroize(connection->io, sizeof(connection->io));
    return sent;
}

static bool enable_control_cipher(airplay_rtsp_server_t *server,
                                  rtsp_connection_t *connection)
{
    if (connection->stream_len != 0U) {
        RTCP_LOGW(TAG, "rtsp_cipher=transition_with_pending_plaintext");
        return false;
    }
    airplay_pair_verify_control_keys_t keys;
    airplay_pair_verify_result_t key_result =
        airplay_rtsp_session_get_control_keys(server->session, &keys);
    if (key_result != AIRPLAY_PAIR_VERIFY_OK ||
        airplay_control_cipher_create(keys.read_key, keys.write_key,
                                      &connection->cipher) !=
            AIRPLAY_CONTROL_CIPHER_OK) {
        mbedtls_platform_zeroize(&keys, sizeof(keys));
        RTCP_LOGE(TAG, "rtsp_cipher=enable_failed result=%d", (int)key_result);
        return false;
    }
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    airplay_rtsp_session_mark_control_cipher_active(server->session);
    RTCP_LOGI(TAG, "rtsp_cipher=enabled");
    return true;
}

static bool process_stream(airplay_rtsp_server_t *server,
                           rtsp_connection_t *connection)
{
    while (connection->stream_len != 0U) {
        airplay_rtsp_parse_result_t parse_result = airplay_rtsp_parse_request(
            connection->stream, connection->stream_len, &connection->request);
        if (parse_result == AIRPLAY_RTSP_PARSE_INCOMPLETE) {
            return connection->stream_len < sizeof(connection->stream);
        }
        if (parse_result != AIRPLAY_RTSP_PARSE_COMPLETE) {
            RTCP_LOGW(TAG, "rtsp_request=rejected parse=%d buffered=%u",
                      (int)parse_result, (unsigned)connection->stream_len);
            return false;
        }

        size_t response_len = 0U;
        bool enable_cipher = false;
        airplay_rtsp_session_result_t result = airplay_rtsp_session_handle(
            server->session, &connection->request, connection->response,
            sizeof(connection->response), &response_len, &enable_cipher);
        if (result != AIRPLAY_RTSP_SESSION_OK || response_len == 0U ||
            !send_response(server, connection, response_len)) {
            RTCP_LOGE(TAG, "rtsp_response=failed result=%d method=%s path=%s",
                      (int)result, connection->request.method,
                      connection->request.path);
            return false;
        }
        size_t consumed = connection->request.consumed;
        consume_stream(connection, consumed);
        RTCP_LOGI(TAG, "rtsp_request=handled method=%s path=%s cseq=%s bytes=%u",
                  connection->request.method, connection->request.path,
                  connection->request.cseq, (unsigned)consumed);
        memset(&connection->request, 0, sizeof(connection->request));

        if (enable_cipher && !enable_control_cipher(server, connection)) {
            return false;
        }
    }
    return true;
}

static bool process_received_bytes(airplay_rtsp_server_t *server,
                                   rtsp_connection_t *connection,
                                   const uint8_t *bytes, size_t length)
{
    if (connection->cipher == NULL) {
        return append_stream(connection, bytes, length) &&
               process_stream(server, connection);
    }

    size_t offset = 0U;
    bool drain_cipher = true;
    while (offset < length || drain_cipher) {
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
             !process_stream(server, connection))) {
            mbedtls_platform_zeroize(connection->response,
                                     sizeof(connection->response));
            return false;
        }
        mbedtls_platform_zeroize(connection->response,
                                 sizeof(connection->response));
        if (result == AIRPLAY_CONTROL_CIPHER_OUTPUT_TOO_SMALL) {
            drain_cipher = true;
            continue;
        }
        if (result == AIRPLAY_CONTROL_CIPHER_OK ||
            result == AIRPLAY_CONTROL_CIPHER_INCOMPLETE) {
            if (offset < length) {
                if (consumed == 0U) return false;
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
    if (server->client_fd < 0 && server->session == NULL) return;
    RTCP_LOGI(TAG, "rtsp_client=disconnected reason=%s", reason);
    if (server->session != NULL) {
        /* Session destroy stops the media endpoints it started. */
        airplay_rtsp_session_destroy(server->session);
        server->session = NULL;
    }
    if (server->connection.cipher != NULL) {
        airplay_control_cipher_destroy(server->connection.cipher);
        server->connection.cipher = NULL;
    }
    if (server->client_fd >= 0) {
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

static void server_accept(airplay_rtsp_server_t *server)
{
    rtcp_addr_t peer;
    memset(&peer, 0, sizeof(peer));
    int client_fd = server->net->accept(server->listener_fd, &peer);
    if (client_fd < 0) {
        RTCP_LOGW(TAG, "rtsp_accept=failed rc=%d", client_fd);
        return;
    }
    server->client_fd = client_fd;
    server->peer = peer;
    server->connection.stream_len = 0U;
    server->connection.cipher = NULL;

    airplay_rtsp_session_config_t session_config = {0};
    session_config.pairing = server->config.pairing;
    session_config.info_plist = server->info_plist;
    session_config.info_plist_len = server->info_plist_len;
    session_config.screen_requested_port = server->config.screen_port;
    session_config.start_screen = server_start_screen;
    session_config.start_event = server_start_event;
    session_config.start_timing = server_start_timing;
    session_config.start_audio = server_start_audio;
    session_config.stop_screen = server_stop_screen;
    session_config.stop_event = server_stop_event;
    session_config.stop_timing = server_stop_timing;
    session_config.stop_audio = server_stop_audio;
    session_config.media_context = server;
    session_config.on_volume = server_volume;
    session_config.volume_context = server;

    if (airplay_rtsp_session_create_borrowing_info_plist(
            &session_config, &server->session) != AIRPLAY_RTSP_SESSION_OK) {
        RTCP_LOGE(TAG, "rtsp_session=create_failed");
        server->session = NULL;
        server_teardown_connection(server, "session_create_failed");
        return;
    }
    RTCP_LOGI(TAG, "rtsp_client=connected peer_port=%u", (unsigned)peer.port);
}

static void server_read_client(airplay_rtsp_server_t *server)
{
    int received = server->net->recv(server->client_fd, server->connection.rx,
                                     sizeof(server->connection.rx));
    if (received == 0) {
        server_teardown_connection(server, "peer_closed");
        return;
    }
    if (received < 0) {
        server_teardown_connection(server, "receive_failed");
        return;
    }
    bool ok = process_received_bytes(server, &server->connection,
                                     server->connection.rx,
                                     (size_t)received);
    mbedtls_platform_zeroize(server->connection.rx,
                             sizeof(server->connection.rx));
    if (!ok) server_teardown_connection(server, "protocol_error");
    else if (server->screen_failed) {
        server_teardown_connection(server, "screen_stream_lost");
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int airplay_rtsp_server_start(const airplay_rtsp_server_config_t *config,
                              airplay_rtsp_server_t **out_server)
{
    if (out_server == NULL) return RTCP_EINVAL;
    *out_server = NULL;
    if (config == NULL || config->os == NULL || config->net == NULL ||
        config->net->listen == NULL || config->net->accept == NULL ||
        config->info_plist == NULL || config->info_plist_len == 0U ||
        config->info_plist_len > AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES ||
        config->on_screen_message == NULL) {
        return RTCP_EINVAL;
    }
    if (config->info_plist_len > sizeof(((airplay_rtsp_server_t *)0)->info_plist)) {
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
        type != AIRPLAY_BPLIST_DICTIONARY) {
        return RTCP_EINVAL;
    }

    uint16_t requested_port = config->port == 0U ? AIRPLAY_RTSP_DEFAULT_PORT
                                                 : config->port;
    int listener_fd = config->net->listen(requested_port);
    if (listener_fd < 0) {
        RTCP_LOGE(TAG, "rtsp_bind=failed rc=%d port=%u", listener_fd,
                  (unsigned)requested_port);
        return listener_fd;
    }

    airplay_rtsp_server_t *server = calloc(1U, sizeof(*server));
    if (server == NULL) {
        config->net->close(listener_fd);
        return RTCP_ENOMEM;
    }
    server->config = *config;
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
    server->port = config->net->local_port != NULL
                       ? config->net->local_port(listener_fd)
                       : requested_port;
    if (server->port == 0U) {
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

bool airplay_rtsp_server_send_hid_report(airplay_rtsp_server_t *server,
                                         uint32_t hid_uid,
                                         const uint8_t *report,
                                         size_t report_len)
{
    if (server == NULL || server->stopping || !server->event_connected) {
        return false;
    }
    return airplay_event_port_send_hid_report(server->event_port, hid_uid,
                                              report, report_len);
}

int airplay_rtsp_server_poll(airplay_rtsp_server_t *server, int timeout_ms)
{
    if (server == NULL) return RTCP_EINVAL;
    if (server->stopping) return RTCP_ESTATE;

    int event_fd = airplay_event_port_get_fd(server->event_port);
    int timing_fd = airplay_timing_port_get_fd(server->timing_port);
    int screen_fd = airplay_screen_server_get_fd(server->screen_server);
    int rtsp_fd = server->client_fd >= 0 ? server->client_fd
                                         : server->listener_fd;

    struct rtcp_pollfd fds[4 + 2 * AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS];
    int count = 0;
    int rtsp_index = -1;
    int event_index = -1;
    int timing_index = -1;
    int screen_index = -1;
    int audio_data_index[AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS];
    int audio_control_index[AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS];
    for (size_t slot = 0U; slot < AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS; ++slot) {
        audio_data_index[slot] = -1;
        audio_control_index[slot] = -1;
    }
    if (rtsp_fd >= 0) {
        rtsp_index = count;
        fds[count].fd = rtsp_fd;
        fds[count].events = RTCP_POLLIN;
        fds[count].revents = 0;
        ++count;
    }
    if (event_fd >= 0) {
        event_index = count;
        fds[count].fd = event_fd;
        fds[count].events = RTCP_POLLIN;
        fds[count].revents = 0;
        ++count;
    }
    if (timing_fd >= 0) {
        timing_index = count;
        fds[count].fd = timing_fd;
        fds[count].events = RTCP_POLLIN;
        fds[count].revents = 0;
        ++count;
    }
    if (screen_fd >= 0) {
        screen_index = count;
        fds[count].fd = screen_fd;
        fds[count].events = RTCP_POLLIN;
        fds[count].revents = 0;
        ++count;
    }
    for (size_t slot = 0U; slot < AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS; ++slot) {
        airplay_audio_stream_t *stream = server->audio[slot];
        if (stream == NULL) continue;
        int data_fd = airplay_audio_stream_get_data_fd(stream);
        int control_fd = airplay_audio_stream_get_control_fd(stream);
        if (data_fd >= 0) {
            audio_data_index[slot] = count;
            fds[count].fd = data_fd;
            fds[count].events = RTCP_POLLIN;
            fds[count].revents = 0;
            ++count;
        }
        if (control_fd >= 0) {
            audio_control_index[slot] = count;
            fds[count].fd = control_fd;
            fds[count].events = RTCP_POLLIN;
            fds[count].revents = 0;
            ++count;
        }
    }

    int ready = count == 0 ? 0 : server->net->poll(fds, count, timeout_ms);
    if (ready < 0) {
        RTCP_LOGW(TAG, "rtsp_poll=failed rc=%d", ready);
        server_teardown_connection(server, "poll_failed");
        return RTCP_EIO;
    }

    /* Timing requests are due on a wall-clock schedule, not on socket
     * readiness, so every iteration services the endpoint. */
    bool timing_readable =
        timing_index >= 0 && (fds[timing_index].revents & RTCP_POLLIN) != 0;
    airplay_timing_port_poll(server->timing_port, timing_readable);

    if (event_index >= 0 && (fds[event_index].revents & RTCP_POLLIN) != 0) {
        airplay_event_port_poll(server->event_port);
    }
    if (screen_index >= 0 && (fds[screen_index].revents & RTCP_POLLIN) != 0) {
        airplay_screen_server_poll(server->screen_server);
    }
    for (size_t slot = 0U; slot < AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS; ++slot) {
        if (server->audio[slot] == NULL) continue;
        bool data_readable = audio_data_index[slot] >= 0 &&
            (fds[audio_data_index[slot]].revents & RTCP_POLLIN) != 0;
        bool control_readable = audio_control_index[slot] >= 0 &&
            (fds[audio_control_index[slot]].revents & RTCP_POLLIN) != 0;
        if (data_readable || control_readable) {
            airplay_audio_stream_poll(server->audio[slot], data_readable,
                                      control_readable);
        }
    }

    if (rtsp_index >= 0 && (fds[rtsp_index].revents != 0)) {
        if (server->client_fd < 0) {
            server_accept(server);
        } else {
            server_read_client(server);
        }
    }

    if (server->screen_failed) {
        server_teardown_connection(server, "screen_stream_lost");
    }
    return RTCP_OK;
}

void airplay_rtsp_server_stop(airplay_rtsp_server_t *server)
{
    if (server == NULL) return;
    server->stopping = true;
    server_teardown_connection(server, "server_stop");
    for (size_t slot = 0U; slot < AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS; ++slot) {
        if (server->audio[slot] != NULL) {
            server_stop_audio((uint16_t)(AIRPLAY_AUDIO_STREAM_MAIN + slot), server);
        }
    }
    if (server->listener_fd >= 0) {
        server->net->close(server->listener_fd);
        server->listener_fd = -1;
    }
    mbedtls_platform_zeroize(server->info_plist, sizeof(server->info_plist));
    memset(&server->config, 0, sizeof(server->config));
    mbedtls_platform_zeroize(server, sizeof(*server));
    free(server);
}
