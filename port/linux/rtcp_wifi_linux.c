/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Linux Wi-Fi seam stub.  SoftAP creation (hostapd) and vendor IE injection
 * are deployment-specific; the board port provides them.  The simulator
 * returns RTCP_ENOTSUP so tests cannot mistake the stub for a working AP.
 */
#include "rtcp_linux.h"

#include "rtcp/rtcp.h"

static int wifi_ap_start(const char *ssid, const char *pass, uint8_t channel)
{
    (void)ssid;
    (void)pass;
    (void)channel;
    return RTCP_ENOTSUP;
}

static int wifi_ap_stop(void)
{
    return RTCP_ENOTSUP;
}

static int wifi_set_vendor_ie(const uint8_t *ie, size_t len)
{
    (void)ie;
    (void)len;
    return RTCP_ENOTSUP;
}

static int wifi_sta_info(struct rtcp_sta_info *out)
{
    (void)out;
    return RTCP_ENOTSUP;
}

const struct rtcp_wifi rtcp_wifi_linux = {
    .ap_start = wifi_ap_start,
    .ap_stop = wifi_ap_stop,
    .set_vendor_ie = wifi_set_vendor_ie,
    .sta_info = wifi_sta_info,
};
