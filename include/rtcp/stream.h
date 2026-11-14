/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Portable blocking byte stream.
 *
 * Lockdown messages and TLS run over a byte stream, not directly over USBMUX:
 * the mux transport presents a connection as an ordered byte pipe, and a TLS
 * session presents another one on top of it.  Keeping the two-faced interface
 * here lets the protocol core be tested with an in-memory stream and lets the
 * board port supply the USB mux pump or mbedTLS without leaking either into
 * the message layer.
 */
#ifndef RTCP_STREAM_H
#define RTCP_STREAM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* Return the number of bytes read (possibly fewer than requested), 0 when
     * the peer closed the stream, or a negative RTCP_* error.  RTCP_ETIMEDOUT
     * means nothing arrived before the timeout; the stream stays usable. */
    int (*read)(void *context, void *buffer, size_t length,
                uint32_t timeout_ms);
    /* Write the whole buffer.  Returns 0 on success or a negative RTCP_*
     * error. */
    int (*write)(void *context, const void *data, size_t length);
} rtcp_stream_ops_t;

typedef struct {
    const rtcp_stream_ops_t *ops;
    void *context;
} rtcp_stream_t;

#endif /* RTCP_STREAM_H */
