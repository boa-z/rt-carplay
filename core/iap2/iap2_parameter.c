/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Ported from esp32-carplay-web-relay main/app_main.c helpers; the framing
 * rules are shared by the Identity, CarPlay and MFi message builders.
 */
#include "iap2_parameter.h"

#include <string.h>

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

size_t iap2_param_encode(uint8_t *out,
                         size_t capacity,
                         uint16_t id,
                         const uint8_t *payload,
                         size_t payload_len)
{
    if (out == NULL || (payload == NULL && payload_len != 0) ||
        payload_len > UINT16_MAX - 4U || payload_len + 4U > capacity) {
        return 0;
    }
    write_be16(out, (uint16_t)(payload_len + 4U));
    write_be16(out + 2, id);
    if (payload_len != 0) memcpy(out + 4, payload, payload_len);
    return payload_len + 4U;
}

bool iap2_param_append(uint8_t *out,
                       size_t capacity,
                       size_t *used,
                       uint16_t id,
                       const uint8_t *payload,
                       size_t payload_len)
{
    if (used == NULL || *used > capacity) return false;
    size_t written = iap2_param_encode(out + *used, capacity - *used,
                                       id, payload, payload_len);
    if (written == 0) return false;
    *used += written;
    return true;
}

bool iap2_param_append_void(uint8_t *out,
                            size_t capacity,
                            size_t *used,
                            uint16_t id)
{
    return iap2_param_append(out, capacity, used, id, NULL, 0);
}

bool iap2_param_find_single(const uint8_t *body,
                            size_t body_len,
                            uint16_t wanted_id,
                            const uint8_t **payload,
                            size_t *payload_len)
{
    if (body == NULL || payload == NULL || payload_len == NULL) return false;
    size_t offset = 0;
    bool found = false;
    while (offset < body_len) {
        if (body_len - offset < 4U) return false;
        uint16_t parameter_len = read_be16(body + offset);
        if (parameter_len < 4U || parameter_len > body_len - offset) return false;
        if (read_be16(body + offset + 2) == wanted_id) {
            if (found) return false;
            found = true;
            *payload = body + offset + 4;
            *payload_len = parameter_len - 4U;
        }
        offset += parameter_len;
    }
    return found;
}

bool iap2_param_validate(const uint8_t *body, size_t body_len)
{
    if (body == NULL && body_len != 0U) return false;
    size_t offset = 0U;
    while (offset < body_len) {
        if (body_len - offset < 4U) return false;
        uint16_t parameter_len = read_be16(body + offset);
        if (parameter_len < 4U || parameter_len > body_len - offset) return false;
        offset += parameter_len;
    }
    return true;
}
