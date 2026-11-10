/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CarPlay Control client: the reverse connection that makes the iPhone open
 * the AirPlay RTSP session.
 *
 * Once the phone commits to CarPlay it advertises `_carplay-ctrl._tcp` on the
 * accessory's network.  The accessory connects to the advertised endpoint and
 * sends `GET /ctrl-int/1/connect` with its AirPlay receiver device id.  This
 * is exactly what the vendor ZALink stack does on this board
 * (`mdnsd_start_carplay_client_connect`, `/ctrl-int/1/%s`, `Host: carplay`,
 * `AirPlay-Receiver-Device-ID: %llu`) and what DiPlay, LIVI, catplay and
 * carplayd's rx_connect do.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtcp/net.h"

#define CARPLAY_CTRL_CONNECT_PATH "/ctrl-int/1/connect"
/* Vendor ZALink sends "carplay" as the Host header; the phone's control
 * server does not use it for routing. */
#define CARPLAY_CTRL_HOST "carplay"
#define CARPLAY_CTRL_TIMEOUT_MS 1500

/* Parses the AirPlay device id ("AA:BB:CC:DD:EE:FF", colons optional) into
 * the 48-bit value the connect request carries.  Returns false on anything
 * that is not exactly 12 hex digits. */
bool carplay_ctrl_parse_device_id(const char *text, uint64_t *value);

/* Builds the connect request.  user_agent is e.g. "AirPlay/950.7.1".
 * Returns the request length or 0 when an argument is invalid or the buffer
 * is too small. */
size_t carplay_ctrl_build_connect_request(uint64_t receiver_device_id,
                                          const char *user_agent,
                                          uint8_t *output,
                                          size_t output_capacity);

/* Blocking connect-out: TCP connect, send the request, read the HTTP status
 * line (bounded by timeout_ms) and close.  Returns RTCP_OK with *http_status
 * set (e.g. 200) when a status line was received, otherwise a negative RTCP
 * error.  The request carries no secret material. */
int carplay_ctrl_connect(const struct rtcp_net *net, const uint8_t ipv4[4],
                         uint16_t port, uint64_t receiver_device_id,
                         const char *user_agent, int timeout_ms,
                         int *http_status);
