/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Luban-Lite / Realtek RTL8733BS Wi-Fi seam.
 *
 * API notes verified against the SDK:
 *   - AP-only mode runs on the Realtek interface "wlan0"; the RT-Thread
 *     logical device name may differ.
 *   - wext_add_custom_ie() is only implemented by the full-function driver
 *     library (CONFIG_REALTEK_FULL_FNC_MODE); the default library answers
 *     "Command not found" even though the in-tree API compiles.
 *   - The custom IE list cannot be added twice; delete before re-adding.
 */
#include "rtcp_lubanlite.h"

#include <string.h>

#if defined(__has_include)
#if __has_include(<rtconfig.h>)
#include <rtconfig.h>
#endif
#endif

#include "rtcp/rtcp.h"

#ifdef AIC_WLAN_REALTEK

#include <wifi_conf.h>
#include <wifi_util.h>

static const char *s_ifname = "wlan0";

static int wifi_ap_start(const char *ssid, const char *pass, uint8_t channel)
{
    if (ssid == NULL || ssid[0] == '\0') return RTCP_EINVAL;

    int security = RTW_SECURITY_WPA2_AES_PSK;
    int pass_len = 0;
    if (pass == NULL || pass[0] == '\0') {
        security = RTW_SECURITY_OPEN;
        pass = "";
    } else {
        pass_len = (int)strlen(pass);
    }

    if (!wifi_is_up(RTW_AP_INTERFACE)) {
        if (wifi_on(RTW_MODE_AP) != RTW_SUCCESS) return RTCP_EIO;
    }
    if (wifi_start_ap((char *)ssid, security, (char *)pass,
                      (int)strlen(ssid), pass_len,
                      (int)channel) != RTW_SUCCESS) {
        return RTCP_EIO;
    }
    return RTCP_OK;
}

static int wifi_ap_stop(void)
{
    return wifi_off() == RTW_SUCCESS ? RTCP_OK : RTCP_EIO;
}

static int wifi_set_vendor_ie(const uint8_t *ie, size_t len)
{
    if (ie == NULL || len == 0U || len > 255U) return RTCP_EINVAL;

    /* Replacing the list is not supported by the driver: clear it first. */
    (void)wext_del_custom_ie(s_ifname);
    rtw_custom_ie_t custom = {
        .ie = (uint8_t *)ie,
        .type = (uint8_t)(PROBE_RSP | BEACON),
    };
    if (wext_add_custom_ie(s_ifname, &custom, 1) != 0) return RTCP_EIO;
    return RTCP_OK;
}

static int wifi_sta_info(struct rtcp_sta_info *out)
{
    (void)out;
    /* The Realtek AP API does not expose per-station RSSI through a stable
     * public call; board-specific code can extend this later. */
    return RTCP_ENOTSUP;
}

void rtcp_wifi_lubanlite_set_interface(const char *name)
{
    if (name != NULL && name[0] != '\0') s_ifname = name;
}

#else /* !AIC_WLAN_REALTEK */

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

void rtcp_wifi_lubanlite_set_interface(const char *name)
{
    (void)name;
}

#endif /* AIC_WLAN_REALTEK */

const struct rtcp_wifi rtcp_wifi_lubanlite = {
    .ap_start = wifi_ap_start,
    .ap_stop = wifi_ap_stop,
    .set_vendor_ie = wifi_set_vendor_ie,
    .sta_info = wifi_sta_info,
};
