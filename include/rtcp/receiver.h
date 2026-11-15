/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_RECEIVER_H
#define RTCP_RECEIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "rtcp/rtcp.h"
#include "rtcp/platform.h"
#include "rtcp/media.h"
#include "rtcp/events.h"
#include "rtcp/input.h"
#include "rtcp/diagnostics.h"
#include "rtcp/data_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtcp_receiver rtcp_receiver_t;

/* Public host composition. create copies this configuration and initial
 * /info bytes. OS/net tables, identity data and callback contexts remain
 * borrowed and immutable until destroy. No board or protocol-private header
 * is needed to populate this structure. See receiver_compat.h for migration. */
typedef struct {
    rtcp_platform_t platform;
    rtcp_media_backend_t media;
    rtcp_iap2_backend_t iap2;
    uint16_t port;        /* 0 selects the standard RTSP port (7000). */
    uint16_t screen_port; /* 0 requests an ephemeral Screen DataStream port. */
    uint8_t family;       /* 0/RTCP_AF_INET or RTCP_AF_INET6. */
    size_t screen_max_body_bytes; /* 0 selects the protocol default. */
    void (*on_event_state)(bool connected, void *context);
    void *event_state_context;
    rtcp_receiver_event_cb_t on_state;
    void *context;
    rtcp_data_stream_backend_t data_stream;
} rtcp_receiver_config_t;

/* All functions on an instance belong to one ordinary thread. The host
 * queues cross-thread commands and joins its poll worker before destroy.
 * No internal thread or GUI is required. start/stop are idempotent. stop
 * closes protocol endpoints synchronously: no protocol callback follows it.
 * A stopped instance can be restarted with the same borrowed providers. */
int rtcp_receiver_create(const rtcp_receiver_config_t *, rtcp_receiver_t **);
int rtcp_receiver_start(rtcp_receiver_t *);
int rtcp_receiver_poll(rtcp_receiver_t *, int timeout_ms);
int rtcp_receiver_stop(rtcp_receiver_t *);
int rtcp_receiver_destroy(rtcp_receiver_t *);
int rtcp_receiver_get_state(const rtcp_receiver_t *, rtcp_receiver_state_t *);
uint16_t rtcp_receiver_port(const rtcp_receiver_t *);
/* Owner-thread observations, including callbacks on that thread. get_peer
 * clears the output and returns ENOENT when no client is attached. Diagnostics
 * returns zero counters for a stopped Receiver; output is copied, never borrowed. */
int rtcp_receiver_get_peer(const rtcp_receiver_t *, rtcp_addr_t *);
int rtcp_receiver_get_diagnostics(const rtcp_receiver_t *,
                                  rtcp_receiver_diagnostics_t *);
/* Owner-thread A/B diagnostic. Default chunked ControlCipher framing is
 * unchanged. Requires a running Receiver; stop/start resets this setting. */
int rtcp_receiver_set_single_cipher_frame(rtcp_receiver_t *, bool enabled);
/* Owner-thread tunneled iAP2 output, including iAP2 callbacks during poll.
 * Bytes are consumed synchronously. False when the event channel is absent. */
bool rtcp_receiver_send_iap(rtcp_receiver_t *, const uint8_t *, size_t);

/* Owner-thread Type 130 transport, including on_body callbacks. send copies
 * bytes into a bounded queue; success is not phone receipt. EINVAL denotes
 * invalid kind/body, ESTATE an absent connection, ENOMEM a full queue.
 * Status works while stopped (zero counters/idle, registration preserved). */
int rtcp_receiver_send_data_stream(rtcp_receiver_t *, rtcp_data_stream_kind_t,
                                    const uint8_t *, size_t);
int rtcp_receiver_get_data_stream_status(const rtcp_receiver_t *,
    rtcp_data_stream_kind_t, rtcp_data_stream_status_t *);

#ifdef __cplusplus
}
#endif
#endif
