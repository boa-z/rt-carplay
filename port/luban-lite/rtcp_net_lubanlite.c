/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Wired CarPlay network seam: read the IPv6 link-local address that the
 * CherryUSB CDC-NCM host glue ("platform/rtthread/usbh_lwip.c") puts on the
 * RT-Thread netdev "u1".
 *
 * No socket code lives here: the AirPlay net seam owns transport.  This file
 * only answers "is the wired netif up, and which link-local address must
 * CarPlayStartSession advertise?".  lwIP core calls are serialized with the
 * TCP/IP core lock, and the address string is copied out before unlocking
 * because ip6addr_ntoa() returns a shared buffer.
 */
#include "rtcp_lubanlite.h"

#include <string.h>

#ifdef RT_USING_LWIP
#include "lwip/opt.h"
#endif

#if defined(RT_USING_LWIP) && LWIP_IPV6

#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"

/* Must match eth_device_init(&g_cdc_ncm_dev, "u1") in the CherryUSB glue. */
#define WIRED_NETIF_NAME "u1"

static bool wired_netif_is_up(struct netif *netif)
{
    return netif != NULL && netif_is_up(netif) && netif_is_link_up(netif);
}

bool rtcp_lubanlite_wired_net_up(void)
{
    bool up;
    LOCK_TCPIP_CORE();
    up = wired_netif_is_up(netif_find(WIRED_NETIF_NAME));
    UNLOCK_TCPIP_CORE();
    return up;
}

bool rtcp_lubanlite_wired_ipv6(char *address, size_t capacity, bool *valid)
{
    bool found = false;

    if (valid != NULL) *valid = false;
    if (address == NULL || capacity < 2U) return false;
    address[0] = '\0';

    LOCK_TCPIP_CORE();
    struct netif *netif = netif_find(WIRED_NETIF_NAME);
    if (wired_netif_is_up(netif)) {
        for (u8_t i = 0U; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
            const ip6_addr_t *entry = netif_ip6_addr(netif, i);
            if (!ip6_addr_islinklocal(entry)) continue;
            const char *text = ip6addr_ntoa(entry);
            size_t len = strlen(text);
            if (len + 1U > capacity) break;
            memcpy(address, text, len + 1U);
            if (valid != NULL) {
                *valid = ip6_addr_isvalid(netif_ip6_addr_state(netif, i)) != 0;
            }
            found = true;
            break;
        }
    }
    UNLOCK_TCPIP_CORE();
    return found;
}

#else /* no lwIP IPv6 in this build */

bool rtcp_lubanlite_wired_net_up(void)
{
    return false;
}

bool rtcp_lubanlite_wired_ipv6(char *address, size_t capacity, bool *valid)
{
    if (valid != NULL) *valid = false;
    if (address != NULL && capacity != 0U) address[0] = '\0';
    return false;
}

#endif
