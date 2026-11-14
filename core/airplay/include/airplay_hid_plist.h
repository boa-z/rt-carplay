/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Encode the DiPlay hidSendReport dictionary subset as an Apple binary plist. */
bool airplay_hid_plist_encode(uint32_t hid_uid,
                              const uint8_t *report,
                              size_t report_len,
                              uint8_t *output,
                              size_t output_capacity,
                              size_t *output_len);
