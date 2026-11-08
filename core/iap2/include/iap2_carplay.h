/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CarPlay iAP2 message fields follow DiPlay/xcertplay's typed 0x4301 codec.
 * See THIRD_PARTY_NOTICES.md and COPYING for attribution and terms.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *ssid;
    const char *passphrase;
    uint8_t channel;
    uint8_t security_type; /* 0=None, 1=WEP, 2=WPA/WPA2, 3=WPA3 transition, 4=WPA3 */
    const char *const *ip_addresses;
    size_t ip_address_count;
    uint32_t airplay_port;
    const char *device_identifier;
    const char *public_key;
    const char *source_version;
} iap2_carplay_start_session_config_t;

typedef struct {
    const char *ssid;
    const char *passphrase;
    uint8_t channel;
    uint8_t security_type;
    const uint8_t *bssid; /* Optional six-byte SoftAP MAC address. */
} iap2_carplay_wifi_config_t;

/* Builds the bounded CSM body for iAP2 AccessoryWiFiConfigurationInformation (0x5703). */
bool iap2_carplay_build_wifi_configuration(
    const iap2_carplay_wifi_config_t *config,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len);

/* Builds the bounded CSM body for iAP2 CarPlayStartSession (0x4301).
 * String parameters are NUL-terminated as required by the iAP2 string encoding.
 * Identity and network values must come from the active, authorized runtime provider. */
bool iap2_carplay_build_start_session(
    const iap2_carplay_start_session_config_t *config,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len);
