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
    const char *pair_code;      /* NULL generates a random 4-digit PIN */
    uint16_t width_pixels;      /* required display width */
    uint16_t height_pixels;     /* required display height */
    uint16_t rotation_degrees;  /* 0, 90, 180 or 270 */
    const char *seed_path;      /* default "/data/rt-carplay.seed" */
    const char *store_path;     /* default "/data/rt-carplay-pairings.bin" */
    carplay_app_screen_cb_t on_screen; /* NULL = on-board sink if built in */
    void *screen_context;
} carplay_app_config_t;

/* Fills config with the defaults above; width/height are left for the
 * caller (the board knows its panel). */
void carplay_app_config_defaults(carplay_app_config_t *config);

/* Brings the receiver up and starts its poll thread.  Returns RTCP_OK or a
 * negative RTCP error.  Call from ordinary thread context (main or MSH). */
int carplay_app_start(const carplay_app_config_t *config);

/* Asks the poll thread to tear everything down and waits for it. */
int carplay_app_stop(void);

bool carplay_app_is_running(void);

/* Current display geometry and the active pairing PIN ("" before start). */
void carplay_app_display_size(uint16_t *width, uint16_t *height);
const char *carplay_app_pairing_code(void);

/*
 * Input injection in display pixels (phase: RTCP_TOUCH_DOWN/MOVE/UP).  Safe
 * from any thread: events are queued and delivered by the poll thread, which
 * is the only thread allowed to write the event channel.
 */
int carplay_app_touch(uint8_t phase, uint16_t pointer_id,
                      uint16_t x, uint16_t y);
