/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_HID_TOUCH_DESCRIPTOR_BYTES 109U

/* Build the DiPlay-compatible two-contact CarPlay touchscreen descriptor. */
bool airplay_hid_touch_descriptor_build(uint16_t x_max,
                                        uint16_t y_max,
                                        uint8_t *output,
                                        size_t output_capacity,
                                        size_t *output_len);
