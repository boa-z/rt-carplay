/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rtcp/net.h"
#include "rtcp/os.h"

/* AirPlay timing (PTP-like) endpoint over UDP, driven by the caller's poll
 * loop.  It answers PT=210 requests and, when a peer port is configured,
 * sends a PT=210 request once per second and consumes the PT=211 reply. */
typedef struct airplay_timing_port airplay_timing_port_t;

typedef struct {
    /* Peer address; IPv4 or IPv6.  peer.port == 0 disables periodic
     * requests.  An IPv6 link-local peer needs peer.scope_id set to the
     * outgoing netif zone. */
    rtcp_addr_t peer;
} airplay_timing_port_config_t;

int airplay_timing_port_start(const struct rtcp_os *os,
                              const struct rtcp_net *net,
                              const airplay_timing_port_config_t *config,
                              airplay_timing_port_t **out_port);

uint16_t airplay_timing_port_get_port(const airplay_timing_port_t *port);
int airplay_timing_port_get_fd(const airplay_timing_port_t *port);

/* Call from the application loop.  socket_readable must reflect poll() on
 * airplay_timing_port_get_fd(). */
void airplay_timing_port_poll(airplay_timing_port_t *port, bool socket_readable);

void airplay_timing_port_stop(airplay_timing_port_t *port,
                              const struct rtcp_net *net);
