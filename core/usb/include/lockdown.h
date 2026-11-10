/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Minimal lockdownd message support.
 *
 * After USBMUX has opened a stream to the iPhone's lockdown port (62078),
 * client and device exchange XML property lists.  The wired CarPlay bring-up
 * needs the pairing record and then opens com.apple.carkit.service; this
 * file only carries the small subset needed to prove the stream end to end:
 * a GetValue request and the string extraction from its reply.
 *
 * Full plist handling (pair records, sessions, StartService) belongs in a
 * later step; keeping this parser intentionally minimal avoids a dependency
 * on an XML/plist library inside the protocol core.
 */
#ifndef RTCP_LOCKDOWN_H
#define RTCP_LOCKDOWN_H

#include <stddef.h>
#include <stdint.h>

/* Maximum XML size the caller should allocate for a one-key request. */
#define RTCP_LOCKDOWN_REQUEST_MAX 512U

/* Build a lockdownd GetValue request plist.  `label` is the client label
 * (letters, digits, dot, dash and underscore); `key` is the requested key
 * (for example "ProductVersion").  Returns the length, or 0 when the buffer
 * is too small or an argument is not a safe bare token. */
size_t rtcp_lockdown_build_get_value(uint8_t *output, size_t capacity,
                                     const char *label, const char *key);

/* Find <key>key</key> and copy the following <string> value into output
 * (NUL terminated).  Returns RTCP_OK, RTCP_ENOENT when the key is absent and
 * RTCP_EINVAL when the reply is malformed or the key is not a string. */
int rtcp_lockdown_extract_string(const uint8_t *xml, size_t length,
                                 const char *key, char *output,
                                 size_t capacity);

#endif /* RTCP_LOCKDOWN_H */
