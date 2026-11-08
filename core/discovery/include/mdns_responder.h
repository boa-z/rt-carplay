/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal mDNS/Bonjour responder for the AirPlay service.
 *
 * It answers PTR/SRV/TXT/A questions for:
 *   _airplay._tcp.local
 *   <instance>._airplay._tcp.local
 *   <host>.local
 *
 * The packet builder is transport-free so it can be unit tested; open/poll
 * wrap the rtcp_net UDP seam and are meant to be driven from the
 * application's own thread.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtcp/net.h"

#define MDNS_PORT 5353U
#define MDNS_MAX_PACKET 1500U
#define MDNS_MAX_NAME 256U

typedef struct {
    const char *host_name;     /* Required, e.g. "rt-carplay" (no ".local"). */
    const char *instance_name; /* Optional; defaults to host_name. */
    uint8_t ipv4[4];           /* Address announced for the host record. */
    uint16_t port;             /* AirPlay RTSP port. */
    const uint8_t *txt;        /* DNS-SD TXT wire data (airplay_txt_encode). */
    size_t txt_len;
} mdns_responder_config_t;

typedef struct mdns_responder mdns_responder_t;

mdns_responder_t *mdns_responder_create(const mdns_responder_config_t *config);
void mdns_responder_destroy(mdns_responder_t *responder);

/*
 * Builds a reply for one received packet.  Returns the response length, or 0
 * when the packet is not a query for one of our records (or is malformed).
 * *unicast is set when the querier asked for a unicast response (QU bit).
 */
size_t mdns_responder_build_response(const mdns_responder_t *responder,
                                     const uint8_t *query,
                                     size_t query_len,
                                     uint8_t *output,
                                     size_t output_capacity,
                                     bool *unicast);

/* Builds an unsolicited announcement with every record (PTR/SRV/TXT/A). */
size_t mdns_responder_build_announcement(const mdns_responder_t *responder,
                                         uint8_t *output,
                                         size_t output_capacity);

/* Binds :5353 with address reuse and joins 224.0.0.251. */
int mdns_responder_open(mdns_responder_t *responder, const struct rtcp_net *net);
void mdns_responder_close(mdns_responder_t *responder, const struct rtcp_net *net);

/* Polls for one packet up to timeout_ms.  Returns 1 if it answered, 0 on
 * timeout, or a negative RTCP error. */
int mdns_responder_poll(mdns_responder_t *responder, const struct rtcp_net *net,
                        int timeout_ms);
