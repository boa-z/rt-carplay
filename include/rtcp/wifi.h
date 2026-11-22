/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file wifi.h
 * SoftAP transport port. */
#ifndef RTCP_WIFI_H
#define RTCP_WIFI_H

#include <stddef.h>
#include <stdint.h>

/** Copied station address and received signal strength in dBm. */
struct rtcp_sta_info
{
    uint8_t mac[6];
    int8_t rssi;
};

/** Platform SoftAP operations. Configure credentials and vendor IEs
 * on the platform owner before accepting wireless CarPlay connections. */
struct rtcp_wifi
{
    int (*ap_start)(const char *ssid, const char *pass, uint8_t channel);
    int (*ap_stop)(void);
    /** Copy the advertised vendor IE from a caller-owned buffer. */
    int (*set_vendor_ie)(const uint8_t *ie, size_t len);
    int (*sta_info)(struct rtcp_sta_info *out);
};

#endif /* RTCP_WIFI_H */
