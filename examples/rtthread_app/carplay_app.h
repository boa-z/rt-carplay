/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * CarPlay / AirPlay receiver application for RT-Thread + Luban-Lite.
 *
 * It composes the protocol library into a running receiver: Wi-Fi SoftAP
 * with the Apple vendor IE, DHCP, mDNS advertisement, the AirPlay RTSP
 * server (Pair-Setup/Pair-Verify/Screen/Event endpoints) and the touch ->
 * HID input bridge.  Board specifics stay behind the rtcp_lubanlite
 * helpers; the encoded Screen stream is handed to on_screen, which defaults
 * to the on-board MPP/DE video sink when RTCARPLAY_VIDEO is enabled.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Screen sink.  configuration=true announces the H.264 SPS/PPS; otherwise
 * annex_b is one access unit prefixed with a start code.  Called from the
 * poll thread and must return quickly (copy the data, do not block). */
typedef void (*carplay_app_screen_cb_t)(bool configuration,
                                        const uint8_t *annex_b,
                                        size_t annex_b_len,
                                        uint64_t sender_time_ns,
                                        void *context);

typedef struct {
    const char *name;           /* Bonjour/MFi name, default "rt-carplay" */
    const char *model;          /* default "ArtInChip" */
    const char *ssid;           /* default "ArtInChip CarPlay" */
    const char *passphrase;     /* default "88888888"; "" = open network */
    uint8_t channel;            /* default 36 (5 GHz, as the vendor image) */
    const char *pair_code;      /* NULL = "3939" (CarPlay's fixed AirPlay PIN) */
    uint16_t width_pixels;      /* required display width */
    uint16_t height_pixels;     /* required display height */
    uint16_t rotation_degrees;  /* 0, 90, 180 or 270 */
    const char *seed_path;      /* default "/data/rt-carplay.seed" */
    const char *store_path;     /* default "/data/rt-carplay-pairings.bin" */
    carplay_app_screen_cb_t on_screen; /* NULL = on-board sink if built in */
    void *screen_context;
    /* Wired CarPlay: skip the SoftAP/mDNS bring-up and serve AirPlay over
     * IPv6 on the phone's CDC-NCM link.  The address advertised in
     * CarPlayStartSession is read from the "u1" netif on demand. */
    bool wired;
} carplay_app_config_t;

/* Fills config with the defaults above; width/height are left for the
 * caller (the board knows its panel). */
void carplay_app_config_defaults(carplay_app_config_t *config);

/* Brings the receiver up and starts its poll thread.  Returns RTCP_OK or a
 * negative RTCP error.  Call from ordinary thread context (main or MSH). */
int carplay_app_start(const carplay_app_config_t *config);

/* Start with the built-in display geometry: SoftAP + mDNS (wireless) and the
 * IPv6 NCM link without them (wired).  The SDK application and the
 * carplay_start command use these. */
int carplay_app_autostart(void);
int carplay_app_autostart_wired(void);

/* Asks the poll thread to tear everything down and waits for it. */
int carplay_app_stop(void);

bool carplay_app_is_running(void);

/* Current display geometry and the active pairing PIN ("" before start). */
void carplay_app_display_size(uint16_t *width, uint16_t *height);
const char *carplay_app_pairing_code(void);

/*
 * Snapshot of the live wireless configuration for the iAP2 session.  The
 * strings are borrowed from the running receiver and stay valid until
 * carplay_app_stop() tears it down, so the caller must stop before that.
 * Returns RTCP_ESTATE when the receiver is not running (or is stopping).
 * ipv4 is four bytes in network order; public_key_hex is the Ed25519 public
 * key exactly as announced in the mDNS TXT record.  airplay_connected is a
 * snapshot; poll it to pace CarPlay re-invites.
 */
typedef struct {
    const char *name;
    const char *model;
    const char *ssid;
    const char *passphrase;
    uint8_t channel;
    uint8_t security_type; /* 0 = open, 2 = WPA/WPA2 PSK */
    uint16_t airplay_port;
    uint8_t ipv4[4];
    uint8_t bssid[6];
    bool bssid_valid;
    const char *device_id;
    const char *public_key_hex;
    const char *source_version;
    bool mdns_ready;
    bool airplay_connected;
} carplay_app_iap2_info_t;

int carplay_app_iap2_info(carplay_app_iap2_info_t *info);

/*
 * Snapshot of the wired AirPlay endpoint for the iAP2 session.  Unlike the
 * wireless snapshot, no SoftAP exists: ipv6 is the NCM netif's link-local in
 * presentation form (e.g. "fe80::..." without a zone), freshly read at call
 * time which is why it must be called when CarPlayAvailability arrives, after
 * the phone has enumerated its NCM function and DAD completed.
 *
 * Returns RTCP_ESTATE when a wireless receiver is running (or none is),
 * RTCP_ENOENT when the wired receiver is up but the netif has no usable
 * address yet.  Strings are borrowed from the running receiver.
 */
typedef struct {
    const char *device_id;
    const char *public_key_hex;
    const char *source_version;
    uint16_t airplay_port;
    char ipv6[64];
    bool airplay_connected;
} carplay_app_wired_info_t;

int carplay_app_wired_info(carplay_app_wired_info_t *info);

/*
 * One owner for the single Bluetooth link: the echo bring-up (carplay_bt)
 * and the iAP2 session (carplay_iap2) register the same RFCOMM server, so
 * running both would split accepts and corrupt the stream.  Claim before
 * enabling the Bluetooth host; release on teardown.
 */
typedef enum {
    CARPLAY_BT_OWNER_NONE = 0,
    CARPLAY_BT_OWNER_ECHO,
    CARPLAY_BT_OWNER_IAP2,
} carplay_bt_owner_t;

bool carplay_bt_claim(carplay_bt_owner_t owner);
void carplay_bt_release(carplay_bt_owner_t owner);
carplay_bt_owner_t carplay_bt_owner(void);

/*
 * Input injection in display pixels (phase: RTCP_TOUCH_DOWN/MOVE/UP).  Safe
 * from any thread: events are queued and delivered by the poll thread, which
 * is the only thread allowed to write the event channel.
 */
int carplay_app_touch(uint8_t phase, uint16_t pointer_id,
                      uint16_t x, uint16_t y);

/* Last AirPlay pairing-table write result, diagnostic snapshot. */
int carplay_app_pairing_storage_error(void);

void carplay_app_debug_counters(unsigned *received, unsigned *hid_sent, unsigned *hid_failed);

/* Low-priority lifecycle owner only, while not starting/stopping the app. */
void carplay_app_performance_report(void);
