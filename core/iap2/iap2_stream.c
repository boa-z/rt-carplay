/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iap2_stream.h"

#include "rtcp/rtcp.h"

void iap2_stream_rx_init(iap2_stream_rx_t *rx, const rtcp_stream_t *stream)
{
    if (rx == NULL) return;
    rx->stream.ops = (stream != NULL) ? stream->ops : NULL;
    rx->stream.context = (stream != NULL) ? stream->context : NULL;
}

int iap2_stream_rx_step(iap2_stream_rx_t *rx, iap2_session_t *session,
                        uint32_t timeout_ms, uint64_t now_ms)
{
    if (rx == NULL || session == NULL || rx->stream.ops == NULL ||
        rx->stream.ops->read == NULL) {
        return RTCP_EINVAL;
    }

    int received = rx->stream.ops->read(rx->stream.context, rx->buffer,
                                        sizeof(rx->buffer), timeout_ms);
    if (received > (int)sizeof(rx->buffer)) {
        return RTCP_EIO;
    }
    if (received > 0) {
        iap2_session_feed(session, rx->buffer, (size_t)received, now_ms);
        iap2_session_advance(session, now_ms);
        return RTCP_OK;
    }

    /* Timeouts and clean closes still run retransmit/keepalive timers. */
    iap2_session_advance(session, now_ms);
    return received;
}
