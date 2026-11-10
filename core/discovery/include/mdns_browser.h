/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal DNS-SD browser for the AirPlay receiver's CarPlay Control
 * reverse connection.
 *
 * Once an iPhone commits to wireless CarPlay it advertises the
 * `_carplay-ctrl._tcp` service.  The receiver browses that service and sends
 * GET /ctrl-int/1/connect to the resolved endpoint; that connect-out is what
 * makes the phone open the AirPlay RTSP session (carplayd's
 * docs/07_SESSION_LIFECYCLE.md: "rx_connect resolves _carplay-ctrl ->
 * connect-out -> receiver pair-verify -> RECORD"; DiPlay's CarPlayBonjour,
 * LIVI's bonjour.rs and catplay's cp_ctrl_client all do the same, and the
 * vendor ZALink stack on this board implements it in mdns_posix.c).
 *
 * The packet builder and parser are transport-free so they can be unit
 * tested without sockets.  The browser shares the mDNS responder's :5353
 * socket: the application feeds every received packet to
 * mdns_browser_handle_packet() and sends mdns_browser_build_query() output
 * through mdns_responder_send().  That keeps one socket on lwIP (a second
 * bind of :5353 is not portable) and lets the responder keep answering
 * `_airplay._tcp` queries in the same poll loop.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mdns_responder.h"

/* A browse result: the SRV target/port plus the A record address.  The
 * service instance name is stored in full (`<instance>.<service>`), matching
 * the PTR/SRV owner names on the wire. */
typedef struct {
    char instance[MDNS_MAX_NAME];
    char target[MDNS_MAX_NAME];
    uint8_t ipv4[4];
    uint16_t port;
    bool has_port;
    bool has_ipv4;
    uint32_t last_seen_ms;
} mdns_browser_endpoint_t;

#define MDNS_BROWSER_MAX_ENDPOINTS 4
#define MDNS_BROWSER_MAX_HOSTS 8
#define MDNS_BROWSER_QUERY_INTERVAL_MS 2000U
#define MDNS_BROWSER_ENDPOINT_MAX_AGE_MS 60000U

typedef struct mdns_browser mdns_browser_t;

/* service is e.g. "_carplay-ctrl._tcp.local" (a trailing dot is accepted). */
mdns_browser_t *mdns_browser_create(const char *service);
void mdns_browser_destroy(mdns_browser_t *browser);

/* Builds one PTR query for the service with the unicast-response bit set.
 * Returns the packet length or 0 when the buffer is too small. */
size_t mdns_browser_build_query(const mdns_browser_t *browser,
                                uint8_t *output, size_t output_capacity);

/* Parses one received mDNS packet (response only).  from_ipv4 is the packet
 * source address and is used as a fallback when the response carries no A
 * record for the SRV target; it may be NULL.  Returns the number of records
 * that updated the browse state, or 0 when the packet was irrelevant, or a
 * negative RTCP error when it was malformed. */
int mdns_browser_handle_packet(mdns_browser_t *browser, const uint8_t *packet,
                               size_t packet_len, const uint8_t *from_ipv4,
                               uint32_t now_ms);

/* True when a query should be (re)sent: immediately after create, then every
 * MDNS_BROWSER_QUERY_INTERVAL_MS. */
bool mdns_browser_query_due(const mdns_browser_t *browser, uint32_t now_ms);
void mdns_browser_mark_queried(mdns_browser_t *browser, uint32_t now_ms);

/* Most recently seen endpoint that has both an address and a port, or NULL.
 * Entries older than MDNS_BROWSER_ENDPOINT_MAX_AGE_MS are never returned. */
const mdns_browser_endpoint_t *mdns_browser_best(const mdns_browser_t *browser,
                                                 uint32_t now_ms);
size_t mdns_browser_endpoint_count(const mdns_browser_t *browser);
