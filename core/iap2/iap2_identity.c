/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Based on DiPlay's wireless iAP2 IdentificationInformation field mapping.
 * See THIRD_PARTY_NOTICES.md and COPYING for attribution and terms.
 */
#include "iap2_identity.h"

#include <string.h>

static void write_be16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static bool append_parameter(uint8_t *out, size_t capacity, size_t *used,
                             uint16_t id, const uint8_t *payload, size_t payload_len)
{
    if (out == NULL || used == NULL || (payload == NULL && payload_len != 0) ||
        payload_len > UINT16_MAX - 4U || *used > capacity ||
        payload_len + 4U > capacity - *used) {
        return false;
    }
    write_be16(out + *used, (uint16_t)(payload_len + 4U));
    write_be16(out + *used + 2U, id);
    if (payload_len != 0) {
        memcpy(out + *used + 4U, payload, payload_len);
    }
    *used += payload_len + 4U;
    return true;
}

static bool append_string(uint8_t *out, size_t capacity, size_t *used,
                          uint16_t id, const char *value)
{
    if (value == NULL) {
        return false;
    }
    size_t len = strlen(value);
    return len < UINT16_MAX - 4U &&
           append_parameter(out, capacity, used, id, (const uint8_t *)value, len + 1U);
}

static bool append_u8(uint8_t *out, size_t capacity, size_t *used, uint16_t id, uint8_t value)
{
    return append_parameter(out, capacity, used, id, &value, 1);
}

static bool append_u16(uint8_t *out, size_t capacity, size_t *used, uint16_t id, uint16_t value)
{
    uint8_t encoded[2];
    write_be16(encoded, value);
    return append_parameter(out, capacity, used, id, encoded, sizeof(encoded));
}

static bool append_void(uint8_t *out, size_t capacity, size_t *used, uint16_t id)
{
    return append_parameter(out, capacity, used, id, NULL, 0);
}

static bool append_group(uint8_t *out, size_t capacity, size_t *used, uint16_t id,
                         const uint8_t *group, size_t group_len)
{
    return append_parameter(out, capacity, used, id, group, group_len);
}

static bool append_u16_list(uint8_t *out, size_t capacity, size_t *used,
                            uint16_t id, const uint16_t *values, size_t count)
{
    uint8_t encoded[32];
    if (count > sizeof(encoded) / 2U) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        write_be16(encoded + i * 2U, values[i]);
    }
    return append_parameter(out, capacity, used, id, encoded, count * 2U);
}

static bool is_message_omitted(uint16_t message_id,
                               const uint16_t *omitted_message_ids,
                               size_t omitted_message_count)
{
    for (size_t i = 0; i < omitted_message_count; ++i) {
        if (omitted_message_ids[i] == message_id) return true;
    }
    return false;
}

static bool is_authentication_message(uint16_t message_id)
{
    return message_id >= 0xaa00U && message_id <= 0xaa05U;
}

bool iap2_identity_parse_unsupported_messages(const uint8_t *body,
                                              size_t body_len,
                                              uint16_t *message_ids,
                                              size_t message_ids_capacity,
                                              size_t *message_id_count)
{
    if (body == NULL || body_len == 0U || message_ids == NULL || message_id_count == NULL ||
        message_ids_capacity == 0U) {
        return false;
    }
    *message_id_count = 0U;
    size_t used = 0U;
    size_t offset = 0U;
    while (offset < body_len) {
        if (body_len - offset < 4U) return false;
        uint16_t parameter_len = (uint16_t)(((uint16_t)body[offset] << 8) | body[offset + 1U]);
        uint16_t parameter_id = (uint16_t)(((uint16_t)body[offset + 2U] << 8) | body[offset + 3U]);
        if (parameter_len < 4U || parameter_len > body_len - offset ||
            (parameter_id != 6U && parameter_id != 7U)) {
            return false;
        }
        size_t payload_len = parameter_len - 4U;
        if ((payload_len & 1U) != 0U) return false;
        for (size_t i = 0U; i < payload_len; i += 2U) {
            uint16_t message_id = (uint16_t)(((uint16_t)body[offset + 4U + i] << 8) |
                                             body[offset + 5U + i]);
            if (is_authentication_message(message_id)) return false;
            bool duplicate = false;
            for (size_t j = 0U; j < used; ++j) {
                if (message_ids[j] == message_id) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                if (used >= message_ids_capacity || used >= IAP2_IDENTITY_MAX_OMITTED_MESSAGES) {
                    return false;
                }
                message_ids[used++] = message_id;
            }
        }
        offset += parameter_len;
    }
    if (used == 0U) return false;
    *message_id_count = used;
    return true;
}

bool iap2_identity_build_wireless(const iap2_identity_config_t *config,
                                  uint8_t *output,
                                  size_t output_capacity,
                                  size_t *output_len)
{
    return iap2_identity_build_wireless_omitting(config, NULL, 0U, output,
                                                 output_capacity, output_len);
}

bool iap2_identity_build_wireless_omitting(const iap2_identity_config_t *config,
                                           const uint16_t *omitted_message_ids,
                                           size_t omitted_message_count,
                                           uint8_t *output,
                                           size_t output_capacity,
                                           size_t *output_len)
{
    if (config == NULL || output == NULL || output_len == NULL ||
        (omitted_message_ids == NULL && omitted_message_count != 0U) ||
        omitted_message_count > IAP2_IDENTITY_MAX_OMITTED_MESSAGES ||
        config->name == NULL || config->model_identifier == NULL ||
        config->manufacturer == NULL || config->serial_number == NULL ||
        config->firmware_version == NULL || config->hardware_version == NULL ||
        config->language == NULL ||
        config->wifi_ssid == NULL || config->wifi_ssid[0] == '\0' ||
        strlen(config->wifi_ssid) > 32U) {
        return false;
    }

    size_t used = 0;
    /* Match DiPlay's wireless accessory message list, excluding its app-specific EA channel. */
    const uint16_t sent_messages[] = {
        0xaa01, 0xaa03, 0x5000, 0x5002, 0x5200, 0x5203, 0xae00, 0xae02,
        0x4157, 0x4159, 0x4154, 0x4156, 0x4301, 0x5703,
    };
    const uint16_t received_messages[] = {
        0xaa00, 0xaa02, 0xaa04, 0xaa05, 0x5001, 0x5201, 0x5202, 0xae01,
        0x4158, 0x4155, 0x4300, 0x4e0d, 0x4e0e, 0x5702,
    };
    uint16_t filtered_sent[sizeof(sent_messages) / sizeof(sent_messages[0])];
    size_t filtered_sent_count = 0U;
    for (size_t i = 0U; i < sizeof(sent_messages) / sizeof(sent_messages[0]); ++i) {
        if (!is_message_omitted(sent_messages[i], omitted_message_ids, omitted_message_count)) {
            filtered_sent[filtered_sent_count++] = sent_messages[i];
        }
    }
    uint16_t filtered_received[sizeof(received_messages) / sizeof(received_messages[0])];
    size_t filtered_received_count = 0U;
    for (size_t i = 0U; i < sizeof(received_messages) / sizeof(received_messages[0]); ++i) {
        if (!is_message_omitted(received_messages[i], omitted_message_ids, omitted_message_count)) {
            filtered_received[filtered_received_count++] = received_messages[i];
        }
    }

    if (!append_string(output, output_capacity, &used, 0, config->name) ||
        !append_string(output, output_capacity, &used, 1, config->model_identifier) ||
        !append_string(output, output_capacity, &used, 2, config->manufacturer) ||
        !append_string(output, output_capacity, &used, 3, config->serial_number) ||
        !append_string(output, output_capacity, &used, 4, config->firmware_version) ||
        !append_string(output, output_capacity, &used, 5, config->hardware_version) ||
        !append_u16_list(output, output_capacity, &used, 6, filtered_sent, filtered_sent_count) ||
        !append_u16_list(output, output_capacity, &used, 7, filtered_received, filtered_received_count) ||
        !append_u8(output, output_capacity, &used, 8, 0) ||
        !append_u16(output, output_capacity, &used, 9, 20)) {
        return false;
    }

    if (!append_string(output, output_capacity, &used, 12, config->language)) {
        return false;
    }

    uint8_t languages[64];
    size_t language_len = strlen(config->language);
    if (language_len + 1U > sizeof(languages)) {
        return false;
    }
    memcpy(languages, config->language, language_len + 1U);
    if (!append_parameter(output, output_capacity, &used, 13, languages, language_len + 1U)) {
        return false;
    }

    uint8_t bluetooth_transport[128];
    size_t bluetooth_transport_len = 0;
    static const uint8_t transport_name[] = "blue";
    if (!append_u16(bluetooth_transport, sizeof(bluetooth_transport), &bluetooth_transport_len, 0, 0) ||
        !append_string(bluetooth_transport, sizeof(bluetooth_transport), &bluetooth_transport_len, 1, "blue") ||
        !append_void(bluetooth_transport, sizeof(bluetooth_transport), &bluetooth_transport_len, 2) ||
        !append_parameter(bluetooth_transport, sizeof(bluetooth_transport), &bluetooth_transport_len,
                          3, config->bluetooth_address, sizeof(config->bluetooth_address)) ||
        !append_parameter(bluetooth_transport, sizeof(bluetooth_transport), &bluetooth_transport_len,
                          4, transport_name, sizeof(transport_name)) ||
        !append_void(bluetooth_transport, sizeof(bluetooth_transport), &bluetooth_transport_len, 5) ||
        !append_group(output, output_capacity, &used, 17,
                      bluetooth_transport, bluetooth_transport_len)) {
        return false;
    }

    uint8_t wifi_transport[128];
    size_t wifi_transport_len = 0;
    if (!append_u16(wifi_transport, sizeof(wifi_transport), &wifi_transport_len, 0, 1) ||
        !append_string(wifi_transport, sizeof(wifi_transport), &wifi_transport_len, 1, config->wifi_ssid) ||
        !append_void(wifi_transport, sizeof(wifi_transport), &wifi_transport_len, 2) ||
        !append_u16(wifi_transport, sizeof(wifi_transport), &wifi_transport_len, 3, 1) ||
        !append_void(wifi_transport, sizeof(wifi_transport), &wifi_transport_len, 4) ||
        !append_void(wifi_transport, sizeof(wifi_transport), &wifi_transport_len, 5) ||
        !append_group(output, output_capacity, &used, 24, wifi_transport, wifi_transport_len)) {
        return false;
    }

    uint8_t route_guidance[96];
    size_t route_guidance_len = 0U;
    if (!append_u16(route_guidance, sizeof(route_guidance), &route_guidance_len, 0, 42) ||
        !append_string(route_guidance, sizeof(route_guidance), &route_guidance_len, 1,
                       "RouteGuidance") ||
        !append_u16(route_guidance, sizeof(route_guidance), &route_guidance_len, 2, 64) ||
        !append_u16(route_guidance, sizeof(route_guidance), &route_guidance_len, 4, 64) ||
        !append_u16(route_guidance, sizeof(route_guidance), &route_guidance_len, 6, 8) ||
        !append_group(output, output_capacity, &used, 30,
                      route_guidance, route_guidance_len)) {
        return false;
    }

    *output_len = used;
    return true;
}
