/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 ESP32 CarPlay Web Relay contributors
 * TLV8 item semantics follow DiPlay's Tlv8Codec.kt. See THIRD_PARTY_NOTICES.md.
 */
#include "airplay_tlv8.h"

#include <string.h>

#define TLV8_SEPARATOR_TYPE 0xffU
#define TLV8_MAX_FRAGMENT 255U

airplay_tlv8_result_t airplay_tlv8_get(const uint8_t *buffer,
                                       size_t buffer_len,
                                       uint8_t wanted_type,
                                       uint8_t *value,
                                       size_t value_capacity,
                                       size_t *value_len)
{
    if ((buffer == NULL && buffer_len != 0) || value == NULL || value_len == NULL) {
        return AIRPLAY_TLV8_INVALID_ARGUMENT;
    }
    *value_len = 0;
    if (buffer_len > AIRPLAY_TLV8_MAX_MESSAGE_BYTES) return AIRPLAY_TLV8_TOO_LARGE;
    bool found = false;
    bool previous_is_wanted = false;
    size_t previous_len = 0;
    size_t offset = 0;

    while (offset < buffer_len) {
        if (buffer_len - offset < 2U) return AIRPLAY_TLV8_MALFORMED;
        uint8_t type = buffer[offset];
        size_t item_len = buffer[offset + 1U];
        offset += 2U;
        if (item_len > buffer_len - offset) return AIRPLAY_TLV8_MALFORMED;

        if (type == wanted_type) {
            if (!(previous_is_wanted && previous_len == TLV8_MAX_FRAGMENT)) {
                *value_len = 0;
            }
            if (item_len > value_capacity - *value_len) return AIRPLAY_TLV8_TOO_LARGE;
            if (item_len != 0) {
                memmove(value + *value_len, buffer + offset, item_len);
                *value_len += item_len;
            }
            found = true;
        }
        previous_is_wanted = type == wanted_type;
        previous_len = item_len;
        offset += item_len;
    }
    return found ? AIRPLAY_TLV8_OK : AIRPLAY_TLV8_NOT_FOUND;
}

static bool append_fragment(uint8_t type,
                            const uint8_t *value,
                            size_t fragment_len,
                            uint8_t *output,
                            size_t output_capacity,
                            size_t *used)
{
    size_t limit = output_capacity < AIRPLAY_TLV8_MAX_MESSAGE_BYTES
                       ? output_capacity : AIRPLAY_TLV8_MAX_MESSAGE_BYTES;
    if (*used > limit || limit - *used < 2U + fragment_len) return false;
    output[*used] = type;
    output[*used + 1U] = (uint8_t)fragment_len;
    *used += 2U;
    if (fragment_len != 0) {
        memcpy(output + *used, value, fragment_len);
        *used += fragment_len;
    }
    return true;
}

airplay_tlv8_result_t airplay_tlv8_encode(const airplay_tlv8_item_t *items,
                                          size_t item_count,
                                          uint8_t *output,
                                          size_t output_capacity,
                                          size_t *output_len)
{
    if ((items == NULL && item_count != 0) || output == NULL || output_len == NULL) {
        return AIRPLAY_TLV8_INVALID_ARGUMENT;
    }
    *output_len = 0;
    if (item_count > AIRPLAY_TLV8_MAX_ITEMS) return AIRPLAY_TLV8_TOO_LARGE;
    size_t used = 0;
    bool have_previous = false;
    uint8_t previous_type = 0;

    for (size_t i = 0; i < item_count; ++i) {
        const airplay_tlv8_item_t *item = &items[i];
        if (item->type == TLV8_SEPARATOR_TYPE ||
            (item->value == NULL && item->value_len != 0)) {
            return AIRPLAY_TLV8_INVALID_ARGUMENT;
        }
        if (have_previous && item->type == previous_type &&
            !append_fragment(TLV8_SEPARATOR_TYPE, NULL, 0, output,
                             output_capacity, &used)) {
            return AIRPLAY_TLV8_TOO_LARGE;
        }

        size_t offset = 0;
        bool first_fragment = true;
        while (first_fragment || offset < item->value_len) {
            size_t remaining = item->value_len - offset;
            size_t fragment_len = remaining > TLV8_MAX_FRAGMENT
                                      ? TLV8_MAX_FRAGMENT : remaining;
            const uint8_t *fragment = fragment_len == 0 ? NULL : item->value + offset;
            if (!append_fragment(item->type, fragment, fragment_len,
                                 output, output_capacity, &used)) {
                return AIRPLAY_TLV8_TOO_LARGE;
            }
            offset += fragment_len;
            first_fragment = false;
        }
        previous_type = item->type;
        have_previous = true;
    }
    *output_len = used;
    return AIRPLAY_TLV8_OK;
}
