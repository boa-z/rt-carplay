/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airplay_audio.h"
#include "airplay_rtsp_session.h"
#include "airplay_screen.h"
#include "rtcp/net.h"
#include "rtcp/os.h"

#define AIRPLAY_RTSP_DEFAULT_PORT 7000U
#define AIRPLAY_RTSP_SERVER_SCREEN_MAX_BODY_BYTES (512U * 1024U)
#define AIRPLAY_RTSP_SERVER_MAX_AUDIO_STREAMS 3U

/*
 * Single-controller AirPlay RTSP server.  It owns the listening socket, the
 * one RTSP connection (including the ControlCipher transition after
 * Pair-Verify) and the event, timing and Screen endpoints the session asks
 * for.  All work happens inside airplay_rtsp_server_poll(): the server builds
 * its own poll set from the os/net seams, so it drops into an RT-Thread or
 * POSIX application loop without a task of its own.
 *
 * The instance is single-threaded: call every function from the same thread.
 */
typedef struct airplay_rtsp_server airplay_rtsp_server_t;

typedef void (*airplay_rtsp_event_state_cb_t)(bool connected, void *context);

typedef struct {
    const struct rtcp_os *os;
    const struct rtcp_net *net;
    airplay_rtsp_pairing_config_t pairing;

    /* Binary-plist capability document served as GET /info.  The bytes are
     * copied at start and may be released by the caller afterwards. */
    const uint8_t *info_plist;
    size_t info_plist_len;

    uint16_t port;        /* 0 selects AIRPLAY_RTSP_DEFAULT_PORT */
    uint16_t screen_port; /* 0 requests an ephemeral Screen DataStream port */
    size_t screen_max_body_bytes; /* 0 selects the default cap */

    /* Decoded Screen frames (Annex-B H.264, borrowed until return). */
    airplay_screen_message_cb_t on_screen_message;
    void *screen_message_context;

    /* Decrypted LPCM audio (little-endian int16, borrowed until return).
     * NULL disables audio: audio SETUPs are then refused. */
    airplay_audio_pcm_cb_t on_audio_pcm;
    void *audio_pcm_context;

    /* Event-channel connect/disconnect notifications. */
    airplay_rtsp_event_state_cb_t on_event_state;
    void *event_state_context;
} airplay_rtsp_server_config_t;

int airplay_rtsp_server_start(const airplay_rtsp_server_config_t *config,
                              airplay_rtsp_server_t **out_server);
uint16_t airplay_rtsp_server_get_port(const airplay_rtsp_server_t *server);
bool airplay_rtsp_server_is_connected(const airplay_rtsp_server_t *server);
bool airplay_rtsp_server_is_encrypted(const airplay_rtsp_server_t *server);

/* Services the RTSP connection and every media endpoint.  timeout_ms bounds
 * the wait for socket activity; 0 polls and returns immediately. */
int airplay_rtsp_server_poll(airplay_rtsp_server_t *server, int timeout_ms);

/* Sends one encrypted HID report over the event channel.  Returns false when
 * the controller has not attached or the channel has failed.  Like the rest
 * of the API, call it from the poll thread. */
bool airplay_rtsp_server_send_hid_report(airplay_rtsp_server_t *server,
                                         uint32_t hid_uid,
                                         const uint8_t *report,
                                         size_t report_len);

/* Stops the connection and all endpoints.  Must be called from the poll
 * thread; no callback is invoked afterwards. */
void airplay_rtsp_server_stop(airplay_rtsp_server_t *server);
