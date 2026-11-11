/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_timing_port.h"

#include <stdlib.h>
#include <string.h>

#include "airplay_timing.h"
#include "rtcp/log.h"
#include "rtcp/rtcp.h"

#define TAG "airplay_timing"

#define TIMING_REQUEST_INTERVAL_US 1000000U
#define NTP_EPOCH_OFFSET_SECONDS 2208988800ULL

struct airplay_timing_port {
    const struct rtcp_os *os;
    const struct rtcp_net *net;
    int socket_fd;
    uint16_t port;
    rtcp_addr_t peer;
    bool request_pending;
    uint64_t pending_timestamp;
    uint64_t next_request_us;
    uint64_t next_sample_log_us;
};

static uint64_t monotonic_us(const airplay_timing_port_t *port)
{
    if (port->os->now_us != NULL) return port->os->now_us();
    return port->os->now_ms() * 1000U;
}

static uint64_t timing_now_ntp(const airplay_timing_port_t *port)
{
    uint64_t monotonic = monotonic_us(port);
    uint64_t seconds = NTP_EPOCH_OFFSET_SECONDS + monotonic / 1000000U;
    uint64_t fraction = ((monotonic % 1000000U) << 32U) / 1000000U;
    return (seconds << 32U) | fraction;
}

static bool same_peer(const rtcp_addr_t *address, const rtcp_addr_t *peer)
{
    if (address->family != peer->family) return false;
    if (peer->family == RTCP_AF_INET) {
        return memcmp(address->addr, peer->addr, 4U) == 0;
    }
    if (peer->family == RTCP_AF_INET6) {
        return memcmp(address->addr, peer->addr, 16U) == 0;
    }
    return false;
}

void airplay_timing_port_poll(airplay_timing_port_t *port, bool socket_readable)
{
    if (port == NULL || port->socket_fd < 0) return;

    uint8_t packet[AIRPLAY_TIMING_PACKET_BYTES];
    uint64_t now_us = monotonic_us(port);
    if (port->peer.port != 0U && now_us >= port->next_request_us) {
        size_t request_len = 0U;
        uint64_t timestamp = timing_now_ntp(port);
        port->request_pending = false;
        if (airplay_timing_build_request(timestamp, packet, sizeof(packet),
                                         &request_len) &&
            port->net->udp_sendto(port->socket_fd, &port->peer, packet,
                                  request_len) == (int)request_len) {
            port->pending_timestamp = timestamp;
            port->request_pending = true;
        }
        port->next_request_us = now_us + TIMING_REQUEST_INTERVAL_US;
    }

    if (!socket_readable) return;

    uint8_t incoming[AIRPLAY_TIMING_PACKET_BYTES + 1U];
    rtcp_addr_t source;
    memset(&source, 0, sizeof(source));
    int received = port->net->udp_recvfrom(port->socket_fd, incoming,
                                           sizeof(incoming), &source);
    if (received < 0) return;
    if (received != (int)AIRPLAY_TIMING_PACKET_BYTES ||
        !same_peer(&source, &port->peer)) {
        return;
    }

    if (incoming[1] == AIRPLAY_TIMING_PT_REQUEST) {
        uint8_t response[AIRPLAY_TIMING_PACKET_BYTES];
        size_t response_len = 0U;
        uint64_t receive_timestamp = timing_now_ntp(port);
        uint64_t transmit_timestamp = timing_now_ntp(port);
        if (airplay_timing_build_response(incoming, (size_t)received,
                                          receive_timestamp,
                                          transmit_timestamp, response,
                                          sizeof(response), &response_len)) {
            (void)port->net->udp_sendto(port->socket_fd, &source, response,
                                        response_len);
        }
    } else if (incoming[1] == AIRPLAY_TIMING_PT_RESPONSE &&
               port->request_pending) {
        double round_trip_seconds = 0.0;
        double offset_seconds = 0.0;
        if (airplay_timing_parse_response(
                incoming, (size_t)received, port->pending_timestamp,
                timing_now_ntp(port), &round_trip_seconds, &offset_seconds)) {
            /* Keep sampling each second; UART needs only an occasional
             * successful sample, especially while decoding live video. */
            if (now_us >= port->next_sample_log_us) {
                /* Peer uptime offsets can exceed signed-int milliseconds.
                 * Print seconds plus fractional milliseconds without an
                 * out-of-range floating-to-int conversion. */
                double magnitude = offset_seconds < 0.0 ? -offset_seconds : offset_seconds;
                unsigned seconds = (unsigned)magnitude;
                unsigned fraction = (unsigned)((magnitude - seconds) * 1000.0);
                unsigned rtt_ms = round_trip_seconds >= (double)UINT32_MAX / 1000.0 ?
                    UINT32_MAX : (unsigned)(round_trip_seconds * 1000.0);
                RTCP_LOGD(TAG, "clock_sample rtt_ms=%u offset_s=%s%u.%03u",
                          rtt_ms,
                          offset_seconds < 0.0 ? "-" : "", seconds, fraction);
                port->next_sample_log_us = now_us + UINT64_C(30000000);
            }
            port->request_pending = false;
        }
    }
}

int airplay_timing_port_start(const struct rtcp_os *os,
                              const struct rtcp_net *net,
                              const airplay_timing_port_config_t *config,
                              airplay_timing_port_t **out_port)
{
    if (out_port == NULL) return RTCP_EINVAL;
    *out_port = NULL;
    if (os == NULL || net == NULL || net->udp_open == NULL ||
        net->udp_sendto == NULL || net->udp_recvfrom == NULL ||
        config == NULL ||
        (config->peer.family != RTCP_AF_INET &&
         config->peer.family != RTCP_AF_INET6)) {
        return RTCP_EINVAL;
    }

    int fd = net->udp_open(config->peer.family, 0U, false);
    if (fd < 0) {
        RTCP_LOGE(TAG, "udp_open failed rc=%d", fd);
        return fd;
    }

    airplay_timing_port_t *port = calloc(1U, sizeof(*port));
    if (port == NULL) {
        net->close(fd);
        return RTCP_ENOMEM;
    }
    port->os = os;
    port->net = net;
    port->socket_fd = fd;
    port->port = net->local_port != NULL ? net->local_port(fd) : 0U;
    port->peer = config->peer;
    port->next_request_us = monotonic_us(port);
    *out_port = port;
    RTCP_LOGI(TAG, "listening on :%u peer_port=%u", (unsigned)port->port,
              (unsigned)port->peer.port);
    return RTCP_OK;
}

uint16_t airplay_timing_port_get_port(const airplay_timing_port_t *port)
{
    return port == NULL ? 0U : port->port;
}

int airplay_timing_port_get_fd(const airplay_timing_port_t *port)
{
    return port == NULL ? -1 : port->socket_fd;
}

void airplay_timing_port_stop(airplay_timing_port_t *port,
                              const struct rtcp_net *net)
{
    if (port == NULL) return;
    if (port->socket_fd >= 0 && net != NULL && net->close != NULL) {
        net->close(port->socket_fd);
    }
    free(port);
}
