/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airplay_pair_verify.h"
#include "rtcp/net.h"
#include "rtcp/os.h"

/*
 * One-controller TCP listener for the encrypted AirPlay event channel.
 * Call get_fd() from the RTSP server's poll set and feed readable events to
 * poll(); this keeps event I/O on the existing server thread.
 */
typedef struct airplay_event_port airplay_event_port_t;

typedef void (*airplay_event_port_state_cb_t)(bool connected, void *context);

typedef struct {
    airplay_pair_verify_event_keys_t keys;
    uint32_t stream_id;
    uint32_t initial_cseq;
    airplay_event_port_state_cb_t on_state;
    void *context;
} airplay_event_port_config_t;

int airplay_event_port_start(const struct rtcp_os *os,
                             const struct rtcp_net *net,
                             const airplay_event_port_config_t *config,
                             airplay_event_port_t **out_port);

uint16_t airplay_event_port_get_port(const airplay_event_port_t *port);
int airplay_event_port_get_fd(const airplay_event_port_t *port);

void airplay_event_port_poll(airplay_event_port_t *port);
void airplay_event_port_stop(airplay_event_port_t *port,
                             const struct rtcp_net *net);

/* Sends one encrypted HID report to the connected controller.  Returns false
 * before the controller attaches or after a channel failure; the caller may
 * call it from its input thread. */
bool airplay_event_port_send_hid_report(airplay_event_port_t *port,
                                        uint32_t hid_uid,
                                        const uint8_t *report,
                                        size_t report_len);
