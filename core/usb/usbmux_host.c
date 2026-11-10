/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usbmux_host.h"

#include <string.h>

#include "rtcp/log.h"
#include "rtcp/rtcp.h"

#define USBMUX_HOST_TAG "usbmux"

/* The reference implementation never retransmits and relies on the bulk pipe
 * preserving order; keep that model and only bound the SYN wait. */
#define USBMUX_CONNECT_PUMP_MS 100U
#define USBMUX_VERSION_REPLY_MS 2000U

static int send_tcp(rtcp_usbmux_host_t *host,
                    rtcp_usbmux_connection_t *connection, uint8_t flags,
                    const void *payload, size_t payload_length)
{
    size_t frame_length = rtcp_usbmux_build_tcp(
        host->tx_frame, sizeof(host->tx_frame), host->tx_index,
        connection->local_port, connection->remote_port,
        connection->tx_sequence, connection->tx_acknowledgment, flags,
        payload, payload_length);
    if (frame_length == 0U) {
        return RTCP_EINVAL;
    }
    ++host->tx_index;
    return host->transport.send(host->transport.user, host->tx_frame,
                                frame_length);
}

static bool rx_push(rtcp_usbmux_connection_t *connection, const uint8_t *data,
                    size_t length)
{
    if (length > RTCP_USBMUX_CONN_RX_CAPACITY - connection->rx_count) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        connection->rx[connection->rx_tail] = data[i];
        connection->rx_tail =
            (connection->rx_tail + 1U) % RTCP_USBMUX_CONN_RX_CAPACITY;
    }
    connection->rx_count += length;
    return true;
}

static void handle_tcp(rtcp_usbmux_host_t *host, const uint8_t *payload,
                       size_t payload_length)
{
    rtcp_usbmux_tcp_packet_t packet;
    if (rtcp_usbmux_parse_tcp(payload, payload_length, &packet) != RTCP_OK) {
        return;
    }

    for (size_t i = 0U; i < RTCP_USBMUX_MAX_CONNECTIONS; ++i) {
        rtcp_usbmux_connection_t *connection = &host->connections[i];
        if (!connection->used ||
            connection->local_port != packet.destination_port) {
            continue;
        }

        if ((packet.flags & RTCP_USBMUX_TH_SYN) != 0U &&
            (packet.flags & RTCP_USBMUX_TH_ACK) != 0U) {
            ++connection->tx_sequence;
            connection->tx_acknowledgment = packet.sequence + 1U;
            send_tcp(host, connection, RTCP_USBMUX_TH_ACK, NULL, 0U);
            connection->established = true;
            continue;
        }
        if ((packet.flags & RTCP_USBMUX_TH_RST) != 0U) {
            connection->reset = true;
            connection->established = true;
            continue;
        }
        if (packet.payload_length != 0U) {
            connection->tx_acknowledgment +=
                (uint32_t)packet.payload_length;
            if (!rx_push(connection, packet.payload, packet.payload_length)) {
                /* Losing bytes in the middle of a TLS stream is worse than
                 * dropping the stream: tell the peer and let the caller
                 * restart from a fresh connection. */
                RTCP_LOGW(USBMUX_HOST_TAG,
                          "stream %u receive buffer full, resetting",
                          connection->local_port);
                connection->reset = true;
                connection->remote_closed = true;
                send_tcp(host, connection, RTCP_USBMUX_TH_RST, NULL, 0U);
                continue;
            }
            send_tcp(host, connection, RTCP_USBMUX_TH_ACK, NULL, 0U);
        }
        if ((packet.flags & RTCP_USBMUX_TH_FIN) != 0U) {
            ++connection->tx_acknowledgment;
            send_tcp(host, connection, RTCP_USBMUX_TH_ACK, NULL, 0U);
            connection->remote_closed = true;
        }
    }
}

void rtcp_usbmux_init(rtcp_usbmux_host_t *host,
                      const rtcp_usbmux_transport_t *transport)
{
    if (host == NULL) {
        return;
    }
    memset(host, 0, sizeof(*host));
    if (transport != NULL) {
        host->transport = *transport;
    }
    host->next_local_port = 1U;
}

int rtcp_usbmux_start(rtcp_usbmux_host_t *host, uint32_t now_ms)
{
    (void)now_ms;
    if (host == NULL || host->transport.send == NULL ||
        host->transport.recv == NULL) {
        return RTCP_EINVAL;
    }

    size_t length = rtcp_usbmux_build_version(host->tx_frame,
                                              sizeof(host->tx_frame));
    if (length == 0U) {
        return RTCP_EINVAL;
    }
    int ret = host->transport.send(host->transport.user, host->tx_frame,
                                   length);
    if (ret < 0) {
        return ret;
    }
    ++host->tx_index;

    /* The iPhone may answer the version packet, but the reference only reads
     * once with a short timeout and ignores the result. */
    (void)rtcp_usbmux_pump(host, USBMUX_VERSION_REPLY_MS, now_ms);

    length = rtcp_usbmux_build_setup(host->tx_frame, sizeof(host->tx_frame),
                                     0x07U, host->tx_index);
    if (length == 0U) {
        return RTCP_EINVAL;
    }
    ret = host->transport.send(host->transport.user, host->tx_frame, length);
    if (ret < 0) {
        return ret;
    }
    ++host->tx_index;
    host->started = true;
    return RTCP_OK;
}

int rtcp_usbmux_pump(rtcp_usbmux_host_t *host, uint32_t timeout_ms,
                     uint32_t now_ms)
{
    (void)now_ms;
    if (host == NULL || host->transport.recv == NULL) {
        return RTCP_EINVAL;
    }
    if (host->reassembly_length >= sizeof(host->reassembly)) {
        /* A complete frame never fills the buffer; treat it as a lost
         * stream instead of reading into zero space. */
        host->reassembly_length = 0U;
        return RTCP_EIO;
    }

    int received = host->transport.recv(
        host->transport.user, host->reassembly + host->reassembly_length,
        sizeof(host->reassembly) - host->reassembly_length, timeout_ms);
    if (received == RTCP_ETIMEDOUT || received == 0) {
        return 0;
    }
    if (received < 0) {
        return received;
    }
    host->reassembly_length += (size_t)received;

    int frames = 0;
    size_t offset = 0U;
    while (offset < host->reassembly_length) {
        size_t frame_length = 0U;
        size_t payload_length = 0U;
        uint32_t proto = 0U;
        const uint8_t *payload = NULL;
        int parsed = rtcp_usbmux_parse_frame(
            host->reassembly + offset, host->reassembly_length - offset,
            &frame_length, &proto, &payload, &payload_length);
        if (parsed == 0) {
            break;
        }
        if (parsed < 0) {
            RTCP_LOGW(USBMUX_HOST_TAG, "frame desynchronised, dropping");
            host->reassembly_length = 0U;
            return RTCP_EIO;
        }
        if (proto == RTCP_USBMUX_PROTO_TCP) {
            handle_tcp(host, payload, payload_length);
        }
        offset += frame_length;
        ++frames;
    }

    if (offset != 0U) {
        memmove(host->reassembly, host->reassembly + offset,
                host->reassembly_length - offset);
        host->reassembly_length -= offset;
    }
    return frames;
}

static rtcp_usbmux_connection_t *allocate_connection(rtcp_usbmux_host_t *host)
{
    for (size_t i = 0U; i < RTCP_USBMUX_MAX_CONNECTIONS; ++i) {
        if (!host->connections[i].used) {
            rtcp_usbmux_connection_t *connection = &host->connections[i];
            memset(connection, 0, sizeof(*connection));
            connection->local_port = host->next_local_port++;
            if (host->next_local_port == 0U) {
                host->next_local_port = 1U;
            }
            connection->used = true;
            return connection;
        }
    }
    return NULL;
}

int rtcp_usbmux_connect(rtcp_usbmux_host_t *host, uint16_t remote_port,
                        uint32_t now_ms, uint32_t timeout_ms,
                        rtcp_usbmux_connection_t **connection)
{
    if (host == NULL || connection == NULL) {
        return RTCP_EINVAL;
    }
    *connection = NULL;
    rtcp_usbmux_connection_t *candidate = allocate_connection(host);
    if (candidate == NULL) {
        return RTCP_ENOMEM;
    }
    candidate->remote_port = remote_port;

    int ret = send_tcp(host, candidate, RTCP_USBMUX_TH_SYN, NULL, 0U);
    if (ret < 0) {
        candidate->used = false;
        return ret;
    }
    candidate->syn_sent = true;

    for (uint32_t elapsed = 0U; elapsed < timeout_ms;
         elapsed += USBMUX_CONNECT_PUMP_MS) {
        ret = rtcp_usbmux_pump(host, USBMUX_CONNECT_PUMP_MS,
                               now_ms + elapsed);
        if (ret < 0) {
            candidate->used = false;
            return ret;
        }
        if (candidate->reset) {
            candidate->used = false;
            return RTCP_EIO;
        }
        if (candidate->established) {
            *connection = candidate;
            return RTCP_OK;
        }
    }

    candidate->used = false;
    return RTCP_ETIMEDOUT;
}

int rtcp_usbmux_connection_send(rtcp_usbmux_host_t *host,
                                rtcp_usbmux_connection_t *connection,
                                const void *data, size_t length)
{
    if (host == NULL || connection == NULL ||
        !connection->used || !connection->established ||
        connection->remote_closed || connection->reset ||
        (data == NULL && length != 0U)) {
        return RTCP_ESTATE;
    }

    const uint8_t *bytes = data;
    while (length != 0U) {
        size_t chunk = length > RTCP_USBMUX_MAX_PAYLOAD
                           ? RTCP_USBMUX_MAX_PAYLOAD
                           : length;
        int ret = send_tcp(host, connection, RTCP_USBMUX_TH_ACK, bytes, chunk);
        if (ret < 0) {
            return ret;
        }
        connection->tx_sequence += (uint32_t)chunk;
        bytes += chunk;
        length -= chunk;
    }
    return RTCP_OK;
}

int rtcp_usbmux_connection_recv(rtcp_usbmux_host_t *host,
                                rtcp_usbmux_connection_t *connection,
                                void *data, size_t capacity)
{
    (void)host;
    if (connection == NULL || !connection->used ||
        (data == NULL && capacity != 0U)) {
        return RTCP_EINVAL;
    }
    size_t copied = 0U;
    uint8_t *output = data;
    while (copied < capacity && connection->rx_count != 0U) {
        output[copied++] = connection->rx[connection->rx_head];
        connection->rx_head =
            (connection->rx_head + 1U) % RTCP_USBMUX_CONN_RX_CAPACITY;
        --connection->rx_count;
    }
    if (copied != 0U) {
        return (int)copied;
    }
    if (connection->reset) {
        return RTCP_EIO;
    }
    if (connection->remote_closed) {
        return RTCP_ESTATE;
    }
    return 0;
}

void rtcp_usbmux_connection_close(rtcp_usbmux_host_t *host,
                                  rtcp_usbmux_connection_t *connection)
{
    if (host == NULL || connection == NULL || !connection->used) {
        return;
    }
    if (connection->established && !connection->remote_closed &&
        !connection->reset) {
        send_tcp(host, connection, RTCP_USBMUX_TH_FIN | RTCP_USBMUX_TH_ACK,
                 NULL, 0U);
    }
    connection->used = false;
}
