/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtcp/bt.h"
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
 * Application helpers for the SoftAP bring-up.  The Wi-Fi seam above stays
 * free of IP details; these functions belong to the board application.
 * get_mac() reads the Realtek MAC (works after ap_start), get_ipv4() returns
 * the AP netif address once lwIP has configured it (RTCP_ESTATE while it is
 * not ready), and start_dhcpd() (re)starts the lwIP DHCP server on that netif.
 */
int rtcp_wifi_lubanlite_get_mac(uint8_t mac[6]);
int rtcp_wifi_lubanlite_get_ipv4(uint8_t address[4]);
int rtcp_wifi_lubanlite_start_dhcpd(void);

/*
 * Classic BR/EDR + RFCOMM transport backed by the SDK Zephyr host
 * (packages/third-party/zephyr-bluetooth, RTL8733BS over H5).  This is the
 * transport the wireless iAP2 path needs:
 *
 *   rtcp_bt_lubanlite.enable(name, NULL)        BT on, connectable+discoverable
 *   rtcp_bt_lubanlite.rfcomm_listen(0, name, uuid128, &channel)
 *                                               RFCOMM server + SDP record
 *   rtcp_bt_lubanlite.rfcomm_accept(1, &fd)     blocks for one DLC
 *   rtcp_bt_lubanlite.send/recv(fd, ...)        byte stream (recv returns 0
 *                                               when the link closed and
 *                                               RTCP_ETIMEDOUT on idle)
 *
 * The UUID is the 128-bit service class advertised in SDP; pass NULL to
 * advertise the Serial Port profile (bring-up without a phone app).
 * rfcomm_listen() is idempotent while the listener is registered.  The seam
 * supports one link; callers must serialize accept/send/recv on one thread.
 *
 * The transport is build-verified only.  The Zephyr H5 transport, the
 * Realtek firmware download and the discoverable/pairing behavior all need
 * the first board run; see port/luban-lite/README.md.
 */
extern const struct rtcp_bt rtcp_bt_lubanlite;

int rtcp_bt_lubanlite_enable(const char *name, const uint8_t bdaddr[6]);
int rtcp_bt_lubanlite_rfcomm_listen(uint8_t channel, const char *name,
                                    const char *uuid128, uint8_t *out_channel);
int rtcp_bt_lubanlite_rfcomm_accept(int lfd, int *fd);
int rtcp_bt_lubanlite_send(int fd, const void *buf, size_t len);
int rtcp_bt_lubanlite_recv(int fd, void *buf, size_t len);

/*
 * As recv(), but with an explicit idle timeout.  A protocol pump that must
 * run periodic work (iAP2 retransmits/invite pacing) uses a short timeout
 * (50-100 ms) instead of the 500 ms default, so recv() time doubles as the
 * pump tick.  timeout_ms <= 0 polls without blocking.
 */
int rtcp_bt_lubanlite_recv_timeout(int fd, void *buf, size_t len,
                                   int timeout_ms);
void rtcp_bt_lubanlite_set_event_cb(void (*cb)(int ev, void *user), void *user);

/* Drops one accepted link but keeps the listener; the slot is recycled after
 * the disconnect callback runs. */
void rtcp_bt_lubanlite_drop_link(int fd);

/* Drops the current DLC (if any) and unblocks a waiting accept()/recv(). */
void rtcp_bt_lubanlite_disconnect(void);
bool rtcp_bt_lubanlite_is_up(void);
bool rtcp_bt_lubanlite_link_up(void);
uint8_t rtcp_bt_lubanlite_channel(void);
unsigned rtcp_bt_lubanlite_rx_dropped(void);

/*
 * Local controller BR/EDR address in display order (aa:bb:cc:dd:ee:ff, the
 * first byte is the most significant octet).  This is the value iAP2 puts in
 * the BluetoothTransportComponent, not the Wi-Fi MAC.  Requires a successful
 * enable(); returns RTCP_ESTATE before that.
 */
int rtcp_bt_lubanlite_local_bdaddr(uint8_t address[6]);

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
