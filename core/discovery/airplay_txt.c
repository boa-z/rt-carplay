/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Bonjour TXT record set for the AirPlay service.  The field set and the
 * default feature mask match the known-working receiver in
 * esp32-carplay-web-relay (GPL-3.0-or-later); a port may override the mask.
 */
#include "airplay_txt.h"

#include <string.h>
#include <stdio.h>

static bool non_empty(const char *value)
{
    return value != NULL && value[0] != '\0';
}

size_t airplay_txt_build(const airplay_txt_config_t *config,
                         airplay_txt_record_t *records,
                         size_t capacity)
{
    if (config == NULL || records == NULL || capacity < 8U) return 0U;
    if (!non_empty(config->device_id) || !non_empty(config->model) ||
        !non_empty(config->source_version) ||
        !non_empty(config->protocol_version) ||
        !non_empty(config->pairing_id) || !non_empty(config->public_key)) {
        return 0U;
    }

    const char *features = non_empty(config->features) ? config->features
                                                       : AIRPLAY_TXT_DEFAULT_FEATURES;
    const char *flags = non_empty(config->flags) ? config->flags
                                                 : AIRPLAY_TXT_DEFAULT_FLAGS;

    records[0] = (airplay_txt_record_t){"deviceid", config->device_id};
    records[1] = (airplay_txt_record_t){"features", features};
    records[2] = (airplay_txt_record_t){"flags", flags};
    records[3] = (airplay_txt_record_t){"model", config->model};
    records[4] = (airplay_txt_record_t){"srcvers", config->source_version};
    records[5] = (airplay_txt_record_t){"protovers", config->protocol_version};
    records[6] = (airplay_txt_record_t){"pi", config->pairing_id};
    records[7] = (airplay_txt_record_t){"pk", config->public_key};
    return 8U;
}

size_t airplay_txt_encode(const airplay_txt_record_t *records,
                          size_t record_count,
                          uint8_t *output,
                          size_t capacity)
{
    if ((records == NULL && record_count != 0U) ||
        (output == NULL && capacity != 0U)) {
        return 0U;
    }
    size_t used = 0U;
    for (size_t i = 0U; i < record_count; ++i) {
        if (!non_empty(records[i].key) || records[i].value == NULL) return 0U;
        size_t key_len = strlen(records[i].key);
        size_t value_len = strlen(records[i].value);
        size_t entry_len = key_len + 1U + value_len;
        if (entry_len > AIRPLAY_TXT_MAX_RECORD_BYTES ||
            used + 1U + entry_len > capacity) {
            return 0U;
        }
        output[used++] = (uint8_t)entry_len;
        memcpy(output + used, records[i].key, key_len);
        used += key_len;
        output[used++] = '=';
        memcpy(output + used, records[i].value, value_len);
        used += value_len;
    }
    return used;
}

bool airplay_txt_format_features(uint64_t features, char *output, size_t capacity)
{
    if (!output || !capacity) return false;
    int n = snprintf(output, capacity, "0x%x,0x%x", (unsigned)(uint32_t)features,
                     (unsigned)(uint32_t)(features >> 32));
    if (n < 0 || (size_t)n >= capacity) { output[0] = '\0'; return false; }
    return true;
}
