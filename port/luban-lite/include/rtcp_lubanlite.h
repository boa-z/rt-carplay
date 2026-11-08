/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtcp/wifi.h"

/*
 * Realtek RTL8733BS SoftAP through the Luban-Lite Wi-Fi API.  Vendor IE
 * injection needs CONFIG_REALTEK_FULL_FNC_MODE (the SetCusIE handler lives
 * in the full-function driver library).
 */
extern const struct rtcp_wifi rtcp_wifi_lubanlite;

/* Realtek interface carrying the AP ("wlan0" for AP-only mode). */
void rtcp_wifi_lubanlite_set_interface(const char *name);

/*
 * Fills output with bytes from the ArtInChip CE true random number generator.
 * Usable as struct rtcp_os.random.  Returns false when the CE driver is not
 * built in or the generator fails/times out; callers must then refuse to
 * start pairing rather than substitute a weaker source.
 *
 * The completion unit is inferred from the CE register layout and is not yet
 * board-verified; see rtcp_random_lubanlite.c and the port README.
 */
bool rtcp_lubanlite_random(uint8_t *output, size_t len);
