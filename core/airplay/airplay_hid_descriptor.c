/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * The report descriptor follows DiPlay AirPlayHid.kt.
 * See THIRD_PARTY_NOTICES.md and COPYING for attribution and terms.
 */
#include "airplay_hid_descriptor.h"

#include <string.h>

static const uint8_t k_application_collection[] = {
    0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01,
};

static const uint8_t k_finger_prefix[] = {
    0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02,
    0x09, 0x38, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x01, 0x09, 0x33, 0x75, 0x01,
    0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03,
};

static const uint8_t k_x_axis_prefix[] = {
    0x05, 0x01, 0x26,
};

static const uint8_t k_x_axis_suffix[] = {
    0x09, 0x30, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
};

static const uint8_t k_y_axis[] = {
    0x26, 0x00, 0x00, 0x09, 0x31, 0x81, 0x02, 0xc0,
};

static bool append(uint8_t *output, size_t capacity, size_t *used,
                   const uint8_t *bytes, size_t length)
{
    if (output == NULL || used == NULL || bytes == NULL || *used > capacity ||
        length > capacity - *used) {
        return false;
    }
    memcpy(output + *used, bytes, length);
    *used += length;
    return true;
}

bool airplay_hid_touch_descriptor_build(uint16_t x_max,
                                        uint16_t y_max,
                                        uint8_t *output,
                                        size_t output_capacity,
                                        size_t *output_len)
{
    static const uint8_t end_collection = 0xc0U;
    size_t used = 0U;
    if (output_len != NULL) *output_len = 0U;
    if (x_max == 0U || y_max == 0U || output == NULL || output_len == NULL ||
        output_capacity < AIRPLAY_HID_TOUCH_DESCRIPTOR_BYTES) {
        return false;
    }
    if (!append(output, output_capacity, &used,
                k_application_collection, sizeof(k_application_collection))) {
        return false;
    }
    for (size_t contact = 0U; contact < 2U; ++contact) {
        uint8_t x_max_bytes[2] = {(uint8_t)x_max, (uint8_t)(x_max >> 8U)};
        uint8_t y_max_bytes[2] = {(uint8_t)y_max, (uint8_t)(y_max >> 8U)};
        if (!append(output, output_capacity, &used,
                    k_finger_prefix, sizeof(k_finger_prefix)) ||
            !append(output, output_capacity, &used,
                    k_x_axis_prefix, sizeof(k_x_axis_prefix)) ||
            !append(output, output_capacity, &used, x_max_bytes,
                    sizeof(x_max_bytes)) ||
            !append(output, output_capacity, &used,
                    k_x_axis_suffix, sizeof(k_x_axis_suffix)) ||
            !append(output, output_capacity, &used, k_y_axis,
                    sizeof(k_y_axis))) {
            return false;
        }
        output[used - sizeof(k_y_axis) + 1U] = y_max_bytes[0];
        output[used - sizeof(k_y_axis) + 2U] = y_max_bytes[1];
    }
    if (!append(output, output_capacity, &used, &end_collection,
                sizeof(end_collection)) ||
        used != AIRPLAY_HID_TOUCH_DESCRIPTOR_BYTES) {
        return false;
    }
    *output_len = used;
    return true;
}
