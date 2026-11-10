/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Minimal XML property-list codec for lockdownd.
 *
 * lockdownd frames every message as a four-byte big-endian length followed by
 * an XML plist whose root value is a dictionary.  The wired CarPlay bring-up
 * only needs flat dictionaries of strings, integers, booleans and data (and
 * one nested dictionary for PairingOptions), so this is deliberately not a
 * general plist library:
 *
 *   - the writer produces the document into a caller buffer and fails closed
 *     on capacity or ordering errors;
 *   - the accessors search a received document for one root-level key and
 *     decode a known type, with hard bounds and no dynamic allocation.
 *
 * Binary plists and arrays are out of scope; lockdown does not use them.
 */
#ifndef RTCP_PLIST_H
#define RTCP_PLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTCP_PLIST_MAX_DEPTH 4U

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    uint8_t depth;
    /* Per open dictionary: 0 = expecting a key or </dict>, 1 = expecting the
     * value for the key just written. */
    uint8_t state[RTCP_PLIST_MAX_DEPTH];
    bool started;
    bool closed;
    bool pending_key;
    bool failed;
} rtcp_plist_writer_t;

void rtcp_plist_writer_init(rtcp_plist_writer_t *writer, void *buffer,
                            size_t capacity);

/* Start the root dictionary.  Returns false (and marks the writer failed) when
 * the writer is not in a state that allows it. */
bool rtcp_plist_begin_dict(rtcp_plist_writer_t *writer);
bool rtcp_plist_end_dict(rtcp_plist_writer_t *writer);

bool rtcp_plist_key(rtcp_plist_writer_t *writer, const char *key);
bool rtcp_plist_string(rtcp_plist_writer_t *writer, const char *value);
bool rtcp_plist_integer(rtcp_plist_writer_t *writer, int64_t value);
bool rtcp_plist_boolean(rtcp_plist_writer_t *writer, bool value);
/* `data` is base64 encoded into the document; NULL with length 0 writes an
 * empty <data></data>. */
bool rtcp_plist_data(rtcp_plist_writer_t *writer, const uint8_t *data,
                     size_t length);

/* Close the document.  Returns the document length in bytes and appends a NUL
 * terminator when one more byte fits (not counted).  Returns 0 on error. */
size_t rtcp_plist_finish(rtcp_plist_writer_t *writer);

/* Root-dictionary accessors.  A missing key returns RTCP_ENOENT; a present key
 * whose value has another type returns RTCP_EINVAL.  `xml` need not be NUL
 * terminated.  Strings are copied and NUL terminated. */
int rtcp_plist_get_string(const uint8_t *xml, size_t length, const char *key,
                          char *output, size_t capacity);
int rtcp_plist_get_integer(const uint8_t *xml, size_t length, const char *key,
                           int64_t *value);
int rtcp_plist_get_boolean(const uint8_t *xml, size_t length, const char *key,
                           bool *value);
int rtcp_plist_get_data(const uint8_t *xml, size_t length, const char *key,
                        uint8_t *output, size_t capacity,
                        size_t *output_length);

/* Decode the lockdownd "Error" value: a string, or an integer with the
 * optional "ErrorString" string preferred.  Returns RTCP_ENOENT when the
 * response carries no error. */
int rtcp_plist_get_error(const uint8_t *xml, size_t length, char *output,
                         size_t capacity);

#endif /* RTCP_PLIST_H */
