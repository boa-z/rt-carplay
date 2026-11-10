/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Transport plumbing shared by the wireless and wired iAP2 sessions.
 *
 * The iAP2 link and CSM layers are pure byte streams.  A transport owner
 * (the RFCOMM link thread or the wired carkit link thread) only needs to
 * read bytes and run the session timers; this helper keeps that loop out of
 * the platform code so it can be tested with an in-memory stream.
 */
#pragma once

#include <stdint.h>

#include "iap2_session.h"
#include "rtcp/stream.h"

#define IAP2_STREAM_RX_BYTES 1024U

typedef struct {
    rtcp_stream_t stream;
    uint8_t buffer[IAP2_STREAM_RX_BYTES];
} iap2_stream_rx_t;

void iap2_stream_rx_init(iap2_stream_rx_t *rx, const rtcp_stream_t *stream);

/*
 * Reads at most one chunk and feeds it to the session, then advances the
 * session timers.  Returns:
 *   RTCP_OK         bytes were fed
 *   RTCP_ETIMEDOUT  nothing arrived before timeout_ms; the link stays usable
 *   0               the peer closed the stream; the caller must link_down
 *   <0 (other)      transport failure; the caller must link_down
 */
int iap2_stream_rx_step(iap2_stream_rx_t *rx, iap2_session_t *session,
                        uint32_t timeout_ms, uint64_t now_ms);
