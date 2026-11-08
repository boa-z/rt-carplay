/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_screen_server.h"

#include <stdlib.h>
#include <string.h>

#include "mbedtls/platform_util.h"
#include "rtcp/log.h"
#include "rtcp/rtcp.h"

#define TAG "airplay_screen"

#define SCREEN_INITIAL_BODY_BYTES 4096U

struct airplay_screen_server {
    const struct rtcp_os *os;
    const struct rtcp_net *net;
    int listener_fd;
    int client_fd;
    uint16_t port;
    bool failed;
    bool stop_requested;
    bool exit_notified;

    airplay_screen_stream_t *stream;
    size_t max_body_bytes;
    airplay_screen_message_cb_t on_message;
    void *message_context;
    airplay_screen_server_exit_cb_t on_unexpected_exit;
    void *exit_context;

    uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES];
    size_t header_len;
    uint8_t *body;
    size_t body_capacity;
    size_t body_len;
    size_t body_expected;
};

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static void notify_exit(airplay_screen_server_t *server)
{
    if (server->exit_notified || server->stop_requested) return;
    server->exit_notified = true;
    if (server->on_unexpected_exit != NULL) {
        server->on_unexpected_exit(server->exit_context);
    }
}

static void fail_server(airplay_screen_server_t *server, const char *reason)
{
    if (server->failed) return;
    RTCP_LOGW(TAG, "screen_peer=closing reason=%s", reason);
    if (server->client_fd >= 0) {
        server->net->close(server->client_fd);
        server->client_fd = -1;
    }
    if (server->listener_fd >= 0) {
        server->net->close(server->listener_fd);
        server->listener_fd = -1;
    }
    server->failed = true;
    notify_exit(server);
}

static bool ensure_body(airplay_screen_server_t *server, size_t required)
{
    if (required <= server->body_capacity) return true;
    size_t capacity = server->body_capacity != 0U
                          ? server->body_capacity
                          : (server->max_body_bytes < SCREEN_INITIAL_BODY_BYTES
                                 ? server->max_body_bytes
                                 : SCREEN_INITIAL_BODY_BYTES);
    while (capacity < required) {
        if (capacity >= server->max_body_bytes / 2U) {
            capacity = server->max_body_bytes;
            break;
        }
        capacity *= 2U;
    }
    if (capacity < required || capacity > server->max_body_bytes) return false;

    uint8_t *body = realloc(server->body, capacity);
    if (body == NULL) return false;
    server->body = body;
    server->body_capacity = capacity;
    return true;
}

/*
 * The net seam has blocking sockets, so each poll performs at most one recv().
 * poll() only reports the socket readable when at least one byte is queued,
 * which bounds the call; a second recv() could block waiting for the rest of
 * a frame and stall the RTSP server thread that drives this poll loop.
 */
static void handle_client(airplay_screen_server_t *server)
{
    if (server->header_len < sizeof(server->header)) {
        int received = server->net->recv(
            server->client_fd, server->header + server->header_len,
            sizeof(server->header) - server->header_len);
        if (received <= 0) {
            fail_server(server, received == 0 ? "peer_closed" : "header_read");
            return;
        }
        server->header_len += (size_t)received;
        if (server->header_len < sizeof(server->header)) return;

        server->body_expected = read_le32(server->header);
        if (server->body_expected > server->max_body_bytes ||
            !ensure_body(server, server->body_expected)) {
            fail_server(server, "body_too_large");
            return;
        }
        server->body_len = 0U;
        if (server->body_expected != 0U) return;
    }

    if (server->body_len < server->body_expected) {
        int received = server->net->recv(
            server->client_fd, server->body + server->body_len,
            server->body_expected - server->body_len);
        if (received <= 0) {
            fail_server(server, received == 0 ? "peer_closed" : "body_read");
            return;
        }
        server->body_len += (size_t)received;
        if (server->body_len < server->body_expected) return;
    }

    uint64_t arrival_us = server->os->now_us != NULL
                              ? server->os->now_us()
                              : server->os->now_ms() * 1000U;
    int result = airplay_screen_stream_process(
        server->stream, server->header, server->body, server->body_len,
        server->body_capacity, arrival_us);
    if (result != RTCP_OK) {
        RTCP_LOGW(TAG, "screen_message=discarded opcode=%u rc=%d",
                  (unsigned)server->header[4], result);
        if (result == RTCP_EIO && server->header[4] == 0U) {
            fail_server(server, "frame_authentication_failed");
            return;
        }
    }

    server->header_len = 0U;
    server->body_len = 0U;
    server->body_expected = 0U;
}

int airplay_screen_server_start(const struct rtcp_os *os,
                                const struct rtcp_net *net,
                                const uint8_t output_key[AIRPLAY_SCREEN_KEY_BYTES],
                                const airplay_screen_server_config_t *config,
                                airplay_screen_server_t **out_server)
{
    if (out_server == NULL) return RTCP_EINVAL;
    *out_server = NULL;
    if (os == NULL || net == NULL || output_key == NULL || config == NULL ||
        config->max_body_bytes == 0U || config->on_message == NULL) {
        return RTCP_EINVAL;
    }

    airplay_screen_server_t *server = calloc(1U, sizeof(*server));
    if (server == NULL) return RTCP_ENOMEM;
    server->os = os;
    server->net = net;
    server->listener_fd = -1;
    server->client_fd = -1;
    server->max_body_bytes = config->max_body_bytes;
    server->on_message = config->on_message;
    server->message_context = config->message_context;
    server->on_unexpected_exit = config->on_unexpected_exit;
    server->exit_context = config->exit_context;

    server->stream = airplay_screen_stream_create(
        output_key, config->max_body_bytes, config->on_message,
        config->message_context);
    if (server->stream == NULL) {
        free(server);
        return RTCP_ENOMEM;
    }

    int listener_fd = net->listen(config->requested_port);
    if (listener_fd < 0) {
        airplay_screen_stream_destroy(server->stream);
        free(server);
        return listener_fd;
    }
    server->listener_fd = listener_fd;
    server->port = net->local_port != NULL ? net->local_port(listener_fd) : 0U;
    if (server->port == 0U) {
        net->close(listener_fd);
        airplay_screen_stream_destroy(server->stream);
        free(server);
        return RTCP_EIO;
    }

    *out_server = server;
    RTCP_LOGI(TAG, "screen_listener=ready port=%u max_body=%u",
              (unsigned)server->port, (unsigned)server->max_body_bytes);
    return RTCP_OK;
}

uint16_t airplay_screen_server_get_port(const airplay_screen_server_t *server)
{
    return server == NULL ? 0U : server->port;
}

int airplay_screen_server_get_fd(const airplay_screen_server_t *server)
{
    if (server == NULL || server->failed) return -1;
    if (server->client_fd >= 0) return server->client_fd;
    return server->listener_fd;
}

void airplay_screen_server_poll(airplay_screen_server_t *server)
{
    if (server == NULL || server->failed) return;

    if (server->client_fd < 0) {
        if (server->listener_fd < 0) return;
        int client_fd = server->net->accept(server->listener_fd, NULL);
        if (client_fd < 0) {
            fail_server(server, "accept");
            return;
        }
        server->client_fd = client_fd;
        /* One key epoch per peer: do not accept a second connection. */
        server->net->close(server->listener_fd);
        server->listener_fd = -1;
        RTCP_LOGI(TAG, "screen_peer=connected port=%u", (unsigned)server->port);
        return; /* Let the next readable event supply the first frame. */
    }

    handle_client(server);
}

void airplay_screen_server_stop(airplay_screen_server_t *server,
                                const struct rtcp_net *net)
{
    if (server == NULL) return;
    server->stop_requested = true;
    if (net != NULL && net->close != NULL) {
        if (server->client_fd >= 0) net->close(server->client_fd);
        if (server->listener_fd >= 0) net->close(server->listener_fd);
    }
    airplay_screen_stream_destroy(server->stream);
    if (server->body != NULL) {
        mbedtls_platform_zeroize(server->body, server->body_capacity);
        free(server->body);
    }
    free(server);
}
