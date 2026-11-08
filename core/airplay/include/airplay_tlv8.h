/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bounded TLV8 framing follows DiPlay's Tlv8Codec.kt. See THIRD_PARTY_NOTICES.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_TLV8_MAX_MESSAGE_BYTES 4096U
#define AIRPLAY_TLV8_MAX_ITEMS 32U

typedef enum {
    AIRPLAY_TLV8_OK = 0,
    AIRPLAY_TLV8_NOT_FOUND,
    AIRPLAY_TLV8_MALFORMED,
    AIRPLAY_TLV8_TOO_LARGE,
    AIRPLAY_TLV8_INVALID_ARGUMENT,
} airplay_tlv8_result_t;

typedef struct {
    uint8_t type;
    const uint8_t *value;
    size_t value_len;
} airplay_tlv8_item_t;

/* Decode one type into caller-owned storage. Repeated adjacent 255-byte
 * fragments are concatenated; a repeated item separated by 0xff/0 replaces
 * the earlier value, matching DiPlay's map-based decoder. */
airplay_tlv8_result_t airplay_tlv8_get(const uint8_t *buffer,
                                       size_t buffer_len,
                                       uint8_t wanted_type,
                                       uint8_t *value,
                                       size_t value_capacity,
                                       size_t *value_len);

/* Encode items into a bounded output buffer. Values are fragmented at 255
 * bytes and repeated adjacent item types are separated by 0xff/0. */
airplay_tlv8_result_t airplay_tlv8_encode(const airplay_tlv8_item_t *items,
                                          size_t item_count,
                                          uint8_t *output,
                                          size_t output_capacity,
                                          size_t *output_len);
