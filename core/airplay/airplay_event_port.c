/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_event_port.h"

#include <stdlib.h>
#include <string.h>

#include "airplay_control_port.h"
#include "mbedtls/platform_util.h"
#include "rtcp/log.h"
#include "rtcp/rtcp.h"

#define TAG "airplay_event"

#define EVENT_RX_CHUNK_BYTES 1200U

struct airplay_event_port {
    const struct rtcp_os *os;
    const struct rtcp_net *net;
    int listener_fd;
    int client_fd;
    uint16_t port;
    bool connected_notified;
    bool failed;
    bool keys_ready;
    airplay_pair_verify_event_keys_t keys;
    uint32_t stream_id;
    uint32_t initial_cseq;
    airplay_control_port_t *control;
    airplay_event_port_state_cb_t on_state;
    void *context;
    uint8_t rx[EVENT_RX_CHUNK_BYTES];
};

static void notify_state(airplay_event_port_t *port, bool connected)
{
    if (port->connected_notified == connected) return;
    port->connected_notified = connected;
    if (port->on_state != NULL) port->on_state(connected, port->context);
}

static void close_fd(airplay_event_port_t *port, int *fd)
{
    if (*fd >= 0) {
        port->net->close(*fd);
        *fd = -1;
    }
}

static void fail_event_port(airplay_event_port_t *port, const char *reason)
{
    if (port->failed) return;
    RTCP_LOGW(TAG, "event_connection=failed reason=%s", reason);
    if (port->control != NULL) {
        airplay_control_port_destroy(port->control);
        port->control = NULL;
    }
    mbedtls_platform_zeroize(&port->keys, sizeof(port->keys));
    port->keys_ready = false;
    close_fd(port, &port->client_fd);
    close_fd(port, &port->listener_fd);
    port->failed = true;
    /* A failed channel must be reported so the session cannot reuse the
     * Pair-Verify event keys after their counters were consumed. */
    notify_state(port, false);
}

static bool socket_write(const uint8_t *bytes, size_t len, void *context)
{
    airplay_event_port_t *port = context;
    if (port == NULL || port->client_fd < 0 || bytes == NULL) return false;
    size_t offset = 0U;
    while (offset < len) {
        int sent = port->net->send(port->client_fd, bytes + offset,
                                   len - offset);
        if (sent <= 0) return false;
        offset += (size_t)sent;
    }
    return true;
}

int airplay_event_port_start(const struct rtcp_os *os,
                             const struct rtcp_net *net,
                             const airplay_event_port_config_t *config,
                             airplay_event_port_t **out_port)
{
    if (out_port == NULL) return RTCP_EINVAL;
    *out_port = NULL;
    if (os == NULL || net == NULL || config == NULL) return RTCP_EINVAL;

    uint8_t family = config->family == 0U ? RTCP_AF_INET : config->family;
    if (family != RTCP_AF_INET && family != RTCP_AF_INET6) return RTCP_EINVAL;

    int listener_fd = net->listen(family, 0U);
    if (listener_fd < 0) {
        RTCP_LOGE(TAG, "event_listener=socket_failed rc=%d", listener_fd);
        return listener_fd;
    }

    airplay_event_port_t *port = calloc(1U, sizeof(*port));
    if (port == NULL) {
        net->close(listener_fd);
        return RTCP_ENOMEM;
    }
    port->os = os;
    port->net = net;
    port->listener_fd = listener_fd;
    port->client_fd = -1;
    port->port = net->local_port != NULL ? net->local_port(listener_fd) : 0U;
    port->on_state = config->on_state;
    port->context = config->context;

    if (port->port == 0U) {
        net->close(listener_fd);
        free(port);
        return RTCP_EIO;
    }

    /* The control channel is created on accept: calling the write callback
     * before a client exists would destroy the keys for no reason. */
    memcpy(&port->keys, &config->keys, sizeof(port->keys));
    port->keys_ready = true;
    port->stream_id = config->stream_id;
    port->initial_cseq = config->initial_cseq;

    *out_port = port;
    RTCP_LOGI(TAG, "event_listener=ready port=%u stream_id=%u",
              (unsigned)port->port, (unsigned)config->stream_id);
    return RTCP_OK;
}

uint16_t airplay_event_port_get_port(const airplay_event_port_t *port)
{
    return port == NULL ? 0U : port->port;
}

int airplay_event_port_get_fd(const airplay_event_port_t *port)
{
    if (port == NULL || port->failed) return -1;
    if (port->client_fd >= 0) return port->client_fd;
    return port->listener_fd;
}

void airplay_event_port_poll(airplay_event_port_t *port)
{
    if (port == NULL || port->failed) return;

    if (port->client_fd < 0) {
        if (port->listener_fd < 0) return;
        int client_fd = port->net->accept(port->listener_fd, NULL);
        if (client_fd < 0) {
            fail_event_port(port, "accept");
            return;
        }
        port->client_fd = client_fd;

        if (!port->keys_ready ||
            airplay_control_port_create(
                port->os, port->keys.read_key, port->keys.write_key,
                port->stream_id, port->initial_cseq, socket_write, port,
                &port->control) != RTCP_OK) {
            fail_event_port(port, "control_create");
            return;
        }
        mbedtls_platform_zeroize(&port->keys, sizeof(port->keys));
        port->keys_ready = false;

        close_fd(port, &port->listener_fd);
        RTCP_LOGI(TAG, "event_connected port=%u", (unsigned)port->port);
        notify_state(port, true);
        return;
    }

    int received = port->net->recv(port->client_fd, port->rx, sizeof(port->rx));
    if (received == 0) {
        fail_event_port(port, "peer_closed");
        return;
    }
    if (received < 0) {
        fail_event_port(port, "receive");
        return;
    }
    if (port->control == NULL) {
        fail_event_port(port, "control_missing");
        return;
    }
    if (airplay_control_port_receive_encrypted(port->control, port->rx,
                                               (size_t)received) != RTCP_OK) {
        RTCP_LOGW(TAG, "event_rx_rejected");
        fail_event_port(port, "encrypted_payload");
        return;
    }
    mbedtls_platform_zeroize(port->rx, (size_t)received);
}

bool airplay_event_port_send_hid_report(airplay_event_port_t *port,
                                        uint32_t hid_uid,
                                        const uint8_t *report,
                                        size_t report_len)
{
    if (port == NULL || port->failed || port->control == NULL) return false;
    return airplay_control_port_send_hid_report(port->control, hid_uid, report,
                                                report_len);
}

void airplay_event_port_stop(airplay_event_port_t *port,
                             const struct rtcp_net *net)
{
    if (port == NULL) return;
    if (port->control != NULL) {
        airplay_control_port_destroy(port->control);
        port->control = NULL;
    }
    if (port->connected_notified) notify_state(port, false);
    if (port->client_fd >= 0 && net != NULL && net->close != NULL) {
        net->close(port->client_fd);
    }
    if (port->listener_fd >= 0 && net != NULL && net->close != NULL) {
        net->close(port->listener_fd);
    }
    mbedtls_platform_zeroize(port, sizeof(*port));
    free(port);
}

bool airplay_event_port_send_iap(airplay_event_port_t *port, const uint8_t *data, size_t len)
{
    return port && !port->failed && port->connected_notified &&
        airplay_control_port_send_iap(port->control, data, len);
}

bool airplay_event_port_request_siri(airplay_event_port_t *port)
{
    return port && !port->failed && port->connected_notified && airplay_control_port_request_siri(port->control);
}
