/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtcp/bt.h"
#include "rtcp/mfi.h"
#include "rtcp/usb.h"
#include "rtcp/wifi.h"

/*
 * Wired CarPlay USB host seam, backed by the Luban-Lite CherryUSB host
 * stack.  The INSTANCE is always linked when the SDK option is enabled; the
 * USB1 (or USB0) host controller is initialized by CherryUSB itself at boot.
 *
 *   ops->open(cb, user)   arm the enumeration hook and event callback.  The
 *                         hook only acts while the seam is open, and it only
 *                         runs during enumeration, so the phone should be
 *                         plugged in after open() (or replugged afterwards).
 *   ops->get_device()     copy the descriptor-derived snapshot; RTCP_ESTATE
 *                         when no accepted iPhone is attached.
 *   ops->control_transfer()/bulk_transfer()
 *                         synchronous transfers on the attached device.
 *
 * Only the Apple USBMUX interface is claimed.  bulk_transfer() accepts its
 * bulk IN/OUT address (taken from the descriptor, never hard-coded) and adds
 * the zero-length packet a packet-aligned OUT transfer needs.
 */
extern const rtcp_usb_t rtcp_usb_lubanlite;

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
 * MFi 2.0C authentication coprocessor on the RT-Thread I2C bus.
 *
 * The Apple 2.0C and pin-compatible parts expose a register interface: a
 * write is ONE I2C transfer of [register, data...]
 * and a read is a register write followed by a read, both ending in STOP
 * (the part does not answer a repeated-start combined transfer).  The
 * register engine lives in core/mfi/mfi2c.c; this seam only supplies I2C
 * transactions.  It can read the certificate and request signatures for a
 * 20-byte challenge; the private key never leaves the coprocessor.
 *
 * The demo128-nand-carplay board has the Apple 2.0C on i2c0 at 0x10
 * (0x11 also answers; i2c0 runs at 100 kHz).  The 0x6A device on the same
 * bus is the vendor RJGT204 device lock, not the MFi coprocessor.
 * start() is idempotent and probes once; it returns RTCP_ENOENT when the
 * bus does not exist and the probe error otherwise.  On failure the iAP2
 * session must be created with .mfi = NULL (it then stops after
 * IdentificationInformation).
 */
int rtcp_mfi_lubanlite_start(const char *i2c_bus_name, uint8_t address);
bool rtcp_mfi_lubanlite_ready(void);
const struct rtcp_mfi *rtcp_mfi_lubanlite(void);

/*
 * Lazy variant for the iAP2 session.  The returned ops probe the coprocessor
 * on first use instead of returning NULL after a boot-time probe failure.
 * Board-verified 2026-11-09: the 2.0C answers 0x32 to every register for a
 * while after reset/RTL8733BS firmware download, but reads correctly when the
 * phone connects later, so the session should retry at certificate time.
 * The bus name and address are copied from this call and used for the retry.
 */
const struct rtcp_mfi *rtcp_mfi_lubanlite_lazy(const char *i2c_bus_name,
                                               uint8_t address);

/*
 * Bring-up diagnostic.  Prints the raw bytes read from the coprocessor with
 * the vendor shape (write STOP, 50 ms, read STOP), the immediate shape and
 * the combined repeated-start attempt, then scans 0x08..0x77 and prints every
 * address that ACKs.  Does not start authentication and does not read the
 * certificate body.  Runs from the 'carplay_mfi' shell command.
 */
void rtcp_mfi_lubanlite_dump(const char *i2c_bus_name, uint8_t address);

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
