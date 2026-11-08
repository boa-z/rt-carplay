/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "rtcp/wifi.h"

/*
 * Realtek RTL8733BS SoftAP through the Luban-Lite Wi-Fi API.  Vendor IE
 * injection needs CONFIG_REALTEK_FULL_FNC_MODE (the SetCusIE handler lives
 * in the full-function driver library).
 */
extern const struct rtcp_wifi rtcp_wifi_lubanlite;

/* Realtek interface carrying the AP ("wlan0" for AP-only mode). */
void rtcp_wifi_lubanlite_set_interface(const char *name);
