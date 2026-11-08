#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    const char *model_identifier;
    const char *manufacturer;
    const char *serial_number;
    const char *firmware_version;
    const char *hardware_version;
    const char *language;
    const char *wifi_ssid;
    uint8_t bluetooth_address[6];
} iap2_identity_config_t;

#define IAP2_IDENTITY_MAX_OMITTED_MESSAGES 32U

/* Builds the bounded parameter body for iAP2 0x1d01 wireless IdentificationInformation. */
bool iap2_identity_build_wireless(const iap2_identity_config_t *config,
                                  uint8_t *output,
                                  size_t output_capacity,
                                  size_t *output_len);

/* Builds IdentificationInformation omitting IDs rejected in 0x1d03 from either list. */
bool iap2_identity_build_wireless_omitting(const iap2_identity_config_t *config,
                                           const uint16_t *omitted_message_ids,
                                           size_t omitted_message_count,
                                           uint8_t *output,
                                           size_t output_capacity,
                                           size_t *output_len);

/* Extracts unsupported message IDs from a rejection containing only parameters 6/7. */
bool iap2_identity_parse_unsupported_messages(const uint8_t *body,
                                              size_t body_len,
                                              uint16_t *message_ids,
                                              size_t message_ids_capacity,
                                              size_t *message_id_count);

