/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * iAP2 parameter framing: a big-endian u16 total length (header included),
 * a big-endian u16 parameter id, then the payload.
 */

/* Encodes one parameter.  Returns bytes written, or 0 on overflow. */
size_t iap2_param_encode(uint8_t *out,
                         size_t capacity,
                         uint16_t id,
                         const uint8_t *payload,
                         size_t payload_len);

/* Appends one parameter to an accumulating body. */
bool iap2_param_append(uint8_t *out,
                       size_t capacity,
                       size_t *used,
                       uint16_t id,
                       const uint8_t *payload,
                       size_t payload_len);

/* Appends a parameter with an empty payload. */
bool iap2_param_append_void(uint8_t *out,
                            size_t capacity,
                            size_t *used,
                            uint16_t id);

/* Finds exactly one parameter with the requested id.  Returns false when the
 * list is malformed, the id is absent, or it appears more than once. */
bool iap2_param_find_single(const uint8_t *body,
                            size_t body_len,
                            uint16_t wanted_id,
                            const uint8_t **payload,
                            size_t *payload_len);

/* Validates that the body is a sequence of well-formed parameters. */
bool iap2_param_validate(const uint8_t *body, size_t body_len);
