/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Lockdown TLS client.
 *
 * Once StartSession reports EnableSessionSSL, the lockdown channel and the
 * per-service channels run over TLS.  The client identity is the pair
 * record's root certificate and key; the phone's certificate is not
 * verified, exactly like the wired reference.  Trust comes from pairing,
 * not from the TLS chain.
 *
 * The session is a portable rtcp_stream_t on top of any transport, so the
 * same code serves the mux connection over USB and a host loopback test.
 */
#ifndef RTCP_LOCKDOWN_TLS_H
#define RTCP_LOCKDOWN_TLS_H

#include <stdint.h>

#include "lockdown_pair.h"
#include "rtcp/rtcp.h"
#include "rtcp/stream.h"

/* Applied to every transport read when the caller passes 0. */
#define RTCP_LOCKDOWN_TLS_TIMEOUT_MS 5000U

typedef struct rtcp_lockdown_tls rtcp_lockdown_tls_t;

/* Handshake a TLS client session over `transport`, presenting the pair
 * record's root identity.  `rng` supplies handshake randomness; the board
 * passes its TRNG.  timeout_ms bounds every transport read.
 *
 * On success *tls owns the session and the caller must call
 * rtcp_lockdown_tls_close().  The transport must stay valid until then.
 * Returns RTCP_EINVAL, RTCP_ENOMEM, RTCP_ETIMEDOUT or RTCP_EIO. */
int rtcp_lockdown_tls_open(rtcp_lockdown_tls_t **tls,
                           const rtcp_stream_t *transport,
                           const rtcp_pair_record_t *record,
                           rtcp_pair_rng_fn rng, void *rng_context,
                           uint32_t timeout_ms);

/* Byte stream over the TLS session, valid until close.  A read timeout is
 * RTCP_ETIMEDOUT and leaves the session usable; a read of 0 means the peer
 * sent close_notify or the transport closed. */
rtcp_stream_t rtcp_lockdown_tls_stream(rtcp_lockdown_tls_t *tls);

/* Send close_notify when possible and release the session.  Accepts NULL. */
void rtcp_lockdown_tls_close(rtcp_lockdown_tls_t *tls);

#endif /* RTCP_LOCKDOWN_TLS_H */
