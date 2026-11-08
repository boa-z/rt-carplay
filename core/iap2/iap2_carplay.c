/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 ESP32 CarPlay Web Relay contributors
 * The 0x4301 parameter mapping follows DiPlay/xcertplay. See
 * THIRD_PARTY_NOTICES.md and COPYING for attribution and terms.
 */
#include "iap2_carplay.h"

#include <string.h>

static void write_be16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void write_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
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

static bool append_u8(uint8_t *out, size_t capacity, size_t *used,
                      uint16_t id, uint8_t value)
{
    return append_parameter(out, capacity, used, id, &value, sizeof(value));
}

bool iap2_carplay_build_wifi_configuration(
    const iap2_carplay_wifi_config_t *config,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len)
{
    if (output_len != NULL) {
        *output_len = 0;
    }
    if (config == NULL || output == NULL || output_len == NULL ||
        config->ssid == NULL || config->passphrase == NULL ||
        config->ssid[0] == '\0' || config->channel == 0 || config->channel > 14 ||
        config->security_type > 4 ||
        (config->security_type != 0 && config->passphrase[0] == '\0')) {
        return false;
    }
    size_t ssid_len = strlen(config->ssid);
    size_t passphrase_len = strlen(config->passphrase);
    if (ssid_len > 32U || passphrase_len > 63U ||
        (config->security_type != 0 && passphrase_len < 8U)) {
        return false;
    }

    size_t used = 0;
    if ((config->bssid != NULL &&
         !append_parameter(output, output_capacity, &used, 0, config->bssid, 6U)) ||
        !append_string(output, output_capacity, &used, 1, config->ssid) ||
        !append_string(output, output_capacity, &used, 2, config->passphrase) ||
        !append_u8(output, output_capacity, &used, 3, config->security_type) ||
        !append_u8(output, output_capacity, &used, 4, config->channel)) {
        return false;
    }
    *output_len = used;
    return true;
}

bool iap2_carplay_build_start_session(
    const iap2_carplay_start_session_config_t *config,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len)
{
    if (output_len != NULL) {
        *output_len = 0;
    }
    if (config == NULL || output == NULL || output_len == NULL ||
        config->ssid == NULL || config->passphrase == NULL ||
        config->ip_addresses == NULL || config->ip_address_count == 0 ||
        config->airplay_port == 0 || config->airplay_port > UINT16_MAX ||
        config->device_identifier == NULL || config->public_key == NULL ||
        config->source_version == NULL || config->ssid[0] == '\0' ||
        config->public_key[0] == '\0' || config->source_version[0] == '\0' ||
        config->device_identifier[0] == '\0' || config->channel == 0 ||
        config->channel > 14 || config->security_type > 4 ||
        (config->security_type != 0 && config->passphrase[0] == '\0')) {
        return false;
    }

    size_t ssid_len = strlen(config->ssid);
    size_t passphrase_len = strlen(config->passphrase);
    if (ssid_len > 32U || passphrase_len > 63U ||
        (config->security_type != 0 && passphrase_len < 8U)) {
        return false;
    }

    size_t used = 0;
    uint8_t wireless[1024];
    size_t wireless_len = 0;
    if (!append_string(wireless, sizeof(wireless), &wireless_len, 0, config->ssid) ||
        !append_string(wireless, sizeof(wireless), &wireless_len, 1, config->passphrase) ||
        !append_u8(wireless, sizeof(wireless), &wireless_len, 2, config->channel)) {
        return false;
    }
    for (size_t i = 0; i < config->ip_address_count; ++i) {
        const char *address = config->ip_addresses[i];
        if (address == NULL || address[0] == '\0' || strlen(address) > 45U ||
            !append_string(wireless, sizeof(wireless), &wireless_len, 3, address)) {
            return false;
        }
    }
    if (!append_u8(wireless, sizeof(wireless), &wireless_len, 4, config->security_type) ||
        !append_parameter(output, output_capacity, &used, 1, wireless, wireless_len)) {
        return false;
    }

    uint8_t port[4];
    write_be32(port, config->airplay_port);
    if (!append_parameter(output, output_capacity, &used, 2, port, sizeof(port)) ||
        !append_string(output, output_capacity, &used, 3, config->device_identifier) ||
        !append_string(output, output_capacity, &used, 4, config->public_key) ||
        !append_string(output, output_capacity, &used, 5, config->source_version)) {
        return false;
    }

    *output_len = used;
    return true;
}
