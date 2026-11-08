/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 ESP32 CarPlay Web Relay contributors
 * Binary plist command shape follows DiPlay's BplistCodec.kt and AirPlaySession.kt.
 * See THIRD_PARTY_NOTICES.md and COPYING for attribution and terms.
 */
#include "airplay_hid_plist.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define HID_PLIST_OBJECT_COUNT 7U
#define HID_PLIST_MAX_REPORT_BYTES 128U
#define HID_PLIST_TRAILER_BYTES 32U

static bool append_ascii(uint8_t *output, size_t capacity, size_t *used,
                         const char *text, size_t text_len)
{
    if (text_len >= 15U || *used > capacity || text_len + 1U > capacity - *used) {
        return false;
    }
    output[(*used)++] = (uint8_t)(0x50U | text_len);
    memcpy(output + *used, text, text_len);
    *used += text_len;
    return true;
}

static bool append_data(uint8_t *output, size_t capacity, size_t *used,
                        const uint8_t *data, size_t data_len)
{
    if (*used > capacity || data_len > capacity - *used) return false;
    if (data_len < 15U) {
        if (data_len + 1U > capacity - *used) return false;
        output[(*used)++] = (uint8_t)(0x40U | data_len);
    } else {
        if (data_len > UINT8_MAX || data_len + 3U > capacity - *used) return false;
        output[(*used)++] = 0x4f;
        output[(*used)++] = 0x10;
        output[(*used)++] = (uint8_t)data_len;
    }
    memcpy(output + *used, data, data_len);
    *used += data_len;
    return true;
}

bool airplay_hid_plist_encode(uint32_t hid_uid,
                              const uint8_t *report,
                              size_t report_len,
                              uint8_t *output,
                              size_t output_capacity,
                              size_t *output_len)
{
    static const uint8_t magic[] = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
    static const char *keys[] = {"type", "uuid", "hidReport"};
    static const char command[] = "hidSendReport";
    char uid[9];
    size_t offsets[HID_PLIST_OBJECT_COUNT];
    size_t used = 0;
    if (output_len != NULL) *output_len = 0;
    if (report == NULL || report_len == 0 || report_len > HID_PLIST_MAX_REPORT_BYTES ||
        output == NULL || output_len == NULL || output_capacity < sizeof(magic) + 1U + 6U +
        HID_PLIST_TRAILER_BYTES) {
        return false;
    }
    int uid_len = snprintf(uid, sizeof(uid), "%" PRIx32, hid_uid);
    if (uid_len <= 0 || (size_t)uid_len >= sizeof(uid)) return false;

    memcpy(output, magic, sizeof(magic));
    used = sizeof(magic);

    /* Root dictionary: keys [1,2,3], values [4,5,6], preserving DiPlay order. */
    offsets[0] = used;
    output[used++] = 0xd3;
    for (uint8_t ref = 1; ref < HID_PLIST_OBJECT_COUNT; ++ref) output[used++] = ref;

    for (size_t i = 0; i < 3U; ++i) {
        offsets[1U + i] = used;
        if (!append_ascii(output, output_capacity - HID_PLIST_TRAILER_BYTES,
                          &used, keys[i], strlen(keys[i]))) return false;
    }
    offsets[4] = used;
    if (!append_ascii(output, output_capacity - HID_PLIST_TRAILER_BYTES,
                      &used, command, sizeof(command) - 1U)) return false;
    offsets[5] = used;
    if (!append_ascii(output, output_capacity - HID_PLIST_TRAILER_BYTES,
                      &used, uid, (size_t)uid_len)) return false;
    offsets[6] = used;
    if (!append_data(output, output_capacity - HID_PLIST_TRAILER_BYTES,
                     &used, report, report_len)) return false;

    if (used > UINT8_MAX || output_capacity - used < HID_PLIST_OBJECT_COUNT + HID_PLIST_TRAILER_BYTES) {
        return false;
    }
    size_t offset_table = used;
    for (size_t i = 0; i < HID_PLIST_OBJECT_COUNT; ++i) output[used++] = (uint8_t)offsets[i];

    memset(output + used, 0, HID_PLIST_TRAILER_BYTES);
    output[used + 6U] = 1; /* offset integer width */
    output[used + 7U] = 1; /* object reference width */
    output[used + 15U] = HID_PLIST_OBJECT_COUNT;
    output[used + 31U] = (uint8_t)offset_table;
    used += HID_PLIST_TRAILER_BYTES;
    *output_len = used;
    return true;
}
