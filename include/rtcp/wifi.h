/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_WIFI_H
#define RTCP_WIFI_H

#include <stddef.h>
#include <stdint.h>

struct rtcp_sta_info {
    uint8_t mac[6];
    int8_t  rssi;
};

struct rtcp_wifi {
    int (*ap_start)(const char *ssid, const char *pass, uint8_t channel);
    int (*ap_stop)(void);
    int (*set_vendor_ie)(const uint8_t *ie, size_t len);
    int (*sta_info)(struct rtcp_sta_info *out);
};

#endif /* RTCP_WIFI_H */
