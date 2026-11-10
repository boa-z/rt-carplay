/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Minimal lockdownd message support.
 *
 * After USBMUX has opened a stream to the iPhone's lockdown port (62078),
 * client and device exchange length-prefixed XML property lists.  This file
 * carries the framing, the request builders the wired CarPlay bring-up needs
 * (GetValue, SetValue, Pair, StartSession, StartService) and the matching
 * response accessors.  The XML value codec lives in rtcp/plist.h.
 *
 * Lockdown runs first in plaintext; StartSession turns the same stream into a
 * TLS session, after which StartService is sent over it.  The TLS layer is a
 * separate stream implementation, so this file stays transport- and
 * crypto-agnostic.
 */
#ifndef RTCP_LOCKDOWN_H
#define RTCP_LOCKDOWN_H

#include <stddef.h>
#include <stdint.h>

#include "rtcp/stream.h"

/* Maximum framed XML message accepted on either side.  StartService replies
 * are small; Pair replies carry certificates and an escrow bag. */
#define RTCP_LOCKDOWN_FRAME_MAX (64U * 1024U)
#define RTCP_LOCKDOWN_REQUEST_MAX 1024U

/* Send `request` as one framed message and read one framed reply.  The reply
 * must fit in `reply_capacity`.  `timeout_ms` applies to each stream read.
 * Returns RTCP_OK and sets *reply_length, or a negative RTCP_* error. */
int rtcp_lockdown_exchange(const rtcp_stream_t *stream,
                           const uint8_t *request, size_t request_length,
                           uint8_t *reply, size_t reply_capacity,
                           size_t *reply_length, uint32_t timeout_ms);

/* Builders return the document length, or 0 when an argument is invalid or
 * the buffer is too small.  Strings must be bare tokens where the protocol
 * allows it; identifiers taken from the phone (ports, keys) are validated by
 * the caller. */
size_t rtcp_lockdown_build_get_value(uint8_t *output, size_t capacity,
                                     const char *label, const char *key);
size_t rtcp_lockdown_build_set_value(uint8_t *output, size_t capacity,
                                     const char *label, const char *key,
                                     const char *value);
size_t rtcp_lockdown_build_start_session(uint8_t *output, size_t capacity,
                                         const char *label,
                                         const char *host_id,
                                         const char *system_buid);
size_t rtcp_lockdown_build_start_service(uint8_t *output, size_t capacity,
                                         const char *service);

/* Pair request with a locally generated pair record.  The certificate
 * buffers are the raw PEM bytes exactly as they go on the wire. */
size_t rtcp_lockdown_build_pair(
    uint8_t *output, size_t capacity, const char *label, const char *host_id,
    const char *system_buid, const uint8_t *device_certificate,
    size_t device_certificate_length, const uint8_t *host_certificate,
    size_t host_certificate_length, const uint8_t *root_certificate,
    size_t root_certificate_length);

/* Find <key>key</key> and copy the following <string> value into output
 * (NUL terminated).  Returns RTCP_OK, RTCP_ENOENT when the key is absent and
 * RTCP_EINVAL when the reply is malformed or the key is not a string. */
int rtcp_lockdown_extract_string(const uint8_t *xml, size_t length,
                                 const char *key, char *output,
                                 size_t capacity);

#endif /* RTCP_LOCKDOWN_H */

