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
    host->tx_frame[14] = (uint8_t)(host->rx_index >> 8U);
    host->tx_frame[15] = (uint8_t)host->rx_index;
    if (host->tx_index <= 4U)
        RTCP_LOGI(USBMUX_HOST_TAG, "tx mux=%u port=%u flags=%02x seq=%u ack=%u bytes=%u",
                  (unsigned)host->tx_index, (unsigned)connection->remote_port,
                  (unsigned)flags, (unsigned)connection->tx_sequence,
                  (unsigned)connection->tx_acknowledgment, (unsigned)payload_length);
    uint16_t index = host->tx_index++;
    ++host->tx_attempts;
    int ret = host->transport.send(host->transport.user, host->tx_frame,
                                   frame_length);
    if (ret < 0) {
        ++host->tx_failures;
        host->failed_tx_index = index;
        host->failed_tx_port = connection->remote_port;
        host->failed_tx_flags = flags;
        host->failed_tx_length = (uint32_t)frame_length;
        host->failed_tx_result = ret;
    }
    return ret;
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
    if (host == NULL || host->transport.send == NULL ||
        host->transport.recv == NULL) {
        return RTCP_EINVAL;
    }

    if (host->started) return RTCP_OK;
    host->version_received = false;
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
    /* usbmuxd sends SETUP only after a valid VERSION response. Pump in
     * bounded slices so fragmented replies are assembled within the budget. */
    for (uint32_t elapsed = 0; elapsed < USBMUX_VERSION_REPLY_MS;
         elapsed += USBMUX_CONNECT_PUMP_MS) {
        ret = rtcp_usbmux_pump(host, USBMUX_CONNECT_PUMP_MS, now_ms + elapsed);
        if (ret < 0) return ret;
        if (host->version_received) break;
    }
    RTCP_LOGI(USBMUX_HOST_TAG, "version reply=%u peer=%u.%u bytes=%u frames=%u",
              (unsigned)host->version_received, (unsigned)host->peer_major,
              (unsigned)host->peer_minor, (unsigned)host->rx_bytes,
              (unsigned)host->rx_frames);
    if (!host->version_received) return RTCP_ETIMEDOUT;
    /* Protocol v1 needs different framing throughout; reject explicitly. */
    if (host->peer_major != RTCP_USBMUX_VERSION) return RTCP_EIO;
    host->tx_index = 0U;
    host->rx_index = 0xffffU;

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

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | (uint32_t)data[3];
}

/* Some iOS releases append four bytes to a VERSION reply (and, per DiPlay's
 * capture, to a few TCP replies).  The next mux header starts four bytes
 * later.  Accept that one boundary only when the skipped-to header is
 * plausible; otherwise the stream is really desynchronised. */
static bool plausible_frame_header(const uint8_t *data, size_t length)
{
    if (length < 8U) return false;
    uint32_t proto = read_be32(data);
    uint32_t frame_size = read_be32(data + 4U);
    if (proto != RTCP_USBMUX_PROTO_VERSION && proto != RTCP_USBMUX_PROTO_SETUP &&
        proto != RTCP_USBMUX_PROTO_CONTROL && proto != RTCP_USBMUX_PROTO_TCP) {
        return false;
    }
    if (proto == RTCP_USBMUX_PROTO_VERSION) {
        return frame_size == RTCP_USBMUX_VERSION_PACKET_SIZE;
    }
    size_t minimum = proto == RTCP_USBMUX_PROTO_TCP ? 36U :
                     proto == RTCP_USBMUX_PROTO_SETUP ? 17U : 16U;
    return frame_size >= minimum && frame_size <= (1U << 20);
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
    if ((size_t)received > sizeof(host->reassembly) - host->reassembly_length)
        return RTCP_EIO;
    host->rx_bytes += (uint32_t)received;
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
            const size_t padding = 4U;
            size_t remaining = host->reassembly_length - offset;
            if (remaining > padding &&
                plausible_frame_header(host->reassembly + offset + padding,
                                       remaining - padding)) {
                RTCP_LOGW(USBMUX_HOST_TAG,
                          "skipping optional 4-byte reply padding");
                offset += padding;
                continue;
            }
            const uint8_t *bad = host->reassembly + offset;
            if (remaining >= 8U)
                RTCP_LOGW(USBMUX_HOST_TAG, "invalid header proto=%08x length=%u buffered=%u",
                          (unsigned)read_be32(bad), (unsigned)read_be32(bad + 4U),
                          (unsigned)remaining);
            if (remaining >= 16U)
                RTCP_LOGW(USBMUX_HOST_TAG, "header field=%08x tx=%u rx=%u",
                          (unsigned)read_be32(bad + 8U),
                          (unsigned)((uint16_t)bad[12] << 8U | bad[13]),
                          (unsigned)((uint16_t)bad[14] << 8U | bad[15]));
            RTCP_LOGW(USBMUX_HOST_TAG, "frame desynchronised, dropping");
            host->reassembly_length = 0U;
            return RTCP_EIO;
        }
        if (proto == RTCP_USBMUX_PROTO_VERSION) {
            if (host->started || host->version_received || payload_length != 12U)
                return RTCP_EIO;
            host->peer_major = read_be32(payload);
            host->peer_minor = read_be32(payload + 4U);
            host->version_received = true;
        } else {
            if (!host->version_received) return RTCP_EIO;
            /* Echo the peer's rx_seq as usbmuxd device_data_input does;
             * this is distinct from the TCP stream acknowledgment. */
            const uint8_t *header = host->reassembly + offset;
            host->rx_index = (uint16_t)((uint16_t)header[14] << 8U | header[15]);
            if (host->rx_frames < 3U)
                RTCP_LOGI(USBMUX_HOST_TAG, "rx proto=%u length=%u field=%08x tx=%u rx=%u",
                          (unsigned)proto, (unsigned)frame_length,
                          (unsigned)read_be32(header + 8U),
                          (unsigned)((uint16_t)header[12] << 8U | header[13]),
                          (unsigned)host->rx_index);
            if (proto == RTCP_USBMUX_PROTO_TCP) {
                if (host->rx_frames < 3U)
                    RTCP_LOGI(USBMUX_HOST_TAG, "tcp src=%u dst=%u flags=%02x seq=%u ack=%u",
                              (unsigned)((uint16_t)payload[0] << 8U | payload[1]),
                              (unsigned)((uint16_t)payload[2] << 8U | payload[3]),
                              (unsigned)payload[13], (unsigned)read_be32(payload + 4U),
                              (unsigned)read_be32(payload + 8U));
                handle_tcp(host, payload, payload_length);
            } else if (proto == RTCP_USBMUX_PROTO_CONTROL) {
                ++host->control_count;
                host->control_type = payload_length ? payload[0] : 0U;
                size_t text_length = payload_length ? payload_length - 1U : 0U;
                if (text_length >= sizeof(host->control_text))
                    text_length = sizeof(host->control_text) - 1U;
                for (size_t i = 0; i < text_length; ++i) {
                    uint8_t c = payload[i + 1U];
                    host->control_text[i] = c >= 32U && c <= 126U ? (char)c : '.';
                }
                host->control_text[text_length] = '\0';
                RTCP_LOGW(USBMUX_HOST_TAG, "device control type=%u bytes=%u",
                          (unsigned)host->control_type, (unsigned)payload_length);
                RTCP_LOGW(USBMUX_HOST_TAG, "control text: %s", host->control_text);
            }
        }
        ++host->rx_frames;
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
    if (!host->started) return RTCP_ESTATE;
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

    RTCP_LOGW(USBMUX_HOST_TAG,
              "connect timeout local=%u remote=%u rxbytes=%u frames=%u pending=%u",
              candidate->local_port, candidate->remote_port,
              (unsigned)host->rx_bytes, (unsigned)host->rx_frames,
              (unsigned)host->reassembly_length);
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

int rtcp_usbmux_connection_read_wait(rtcp_usbmux_host_t *host,
                                    rtcp_usbmux_connection_t *connection,
                                    void *data, size_t capacity,
                                    uint32_t timeout_ms,
                                    uint32_t (*clock_ms)(void *), void *clock_user)
{
    if (!host || !clock_ms) return RTCP_EINVAL;
    if (capacity == 0U) return 0;
    uint32_t start = clock_ms(clock_user);
    for (;;) {
        int received = rtcp_usbmux_connection_recv(host, connection, data, capacity);
        if (received != 0) return received;
        uint32_t now = clock_ms(clock_user);
        uint32_t elapsed = now - start;
        if (elapsed >= timeout_ms) return RTCP_ETIMEDOUT;
        int ret = rtcp_usbmux_pump(host, timeout_ms - elapsed, now);
        if (ret < 0) return ret;
    }
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
