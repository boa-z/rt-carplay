/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airplay_audio.h"
#include "airplay_rtsp_session.h"
#include "airplay_screen.h"
#include "airplay_screen_server.h"
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
    /* Address family of the RTSP listener and of every media endpoint it
     * starts: 0/RTCP_AF_INET (default) or RTCP_AF_INET6.  Wired CarPlay uses
     * IPv6 link-local; the phone connects to the address advertised in
     * CarPlayStartSession. */
    uint8_t family;
    size_t screen_max_body_bytes; /* 0 selects the default cap */

    /* Decoded Screen frames (Annex-B H.264, borrowed until return). */
    airplay_screen_message_cb_t on_screen_message;
    void *screen_message_context;

    /* Decrypted LPCM audio (little-endian int16, borrowed until return).
     * NULL disables audio: audio SETUPs are then refused. */
    airplay_audio_pcm_cb_t on_audio_pcm;
    void *audio_pcm_context;

    /* SET_PARAMETER "volume: <dB>" from the sender (0 = full scale).
     * NULL acknowledges the request without changing anything. */
    airplay_rtsp_volume_cb_t on_volume;
    void *volume_context;

    /* Event-channel connect/disconnect notifications. */
    airplay_rtsp_event_state_cb_t on_event_state;
    void *event_state_context;
} airplay_rtsp_server_config_t;

int airplay_rtsp_server_start(const airplay_rtsp_server_config_t *config,
                              airplay_rtsp_server_t **out_server);
uint16_t airplay_rtsp_server_get_port(const airplay_rtsp_server_t *server);
bool airplay_rtsp_server_is_connected(const airplay_rtsp_server_t *server);
bool airplay_rtsp_server_is_encrypted(const airplay_rtsp_server_t *server);

/* Address of the currently attached RTSP client, when it is IPv4 or IPv6.
 * The CarPlay Control browser uses an IPv4 peer to address a unicast mDNS
 * query at the phone; wired sessions read an IPv6 link-local peer here for
 * the timing endpoint. */
bool airplay_rtsp_server_get_peer(const airplay_rtsp_server_t *server,
                                  rtcp_addr_t *peer);

/* Services the RTSP connection and every media endpoint.  timeout_ms bounds
 * the wait for socket activity; 0 polls and returns immediately. */
int airplay_rtsp_server_poll(airplay_rtsp_server_t *server, int timeout_ms);
/* Snapshot on the server thread, like the other server APIs. */
void airplay_rtsp_server_get_screen_stats(const airplay_rtsp_server_t *server,
                                         airplay_screen_server_stats_t *stats);

/* Sends one encrypted HID report over the event channel.  Returns false when
 * the controller has not attached or the channel has failed.  Like the rest
 * of the API, call it from the poll thread. */
bool airplay_rtsp_server_send_hid_report(airplay_rtsp_server_t *server,
                                         uint32_t hid_uid,
                                         const uint8_t *report,
                                         size_t report_len);

/* Wireless A/B diagnostic: seal each whole RTSP reply as one ControlCipher
 * frame instead of Apple's 1024-byte chunking.  Replies larger than the
 * maximum frame stay chunked.  Off by default; safe to flip at runtime from
 * another thread (the poll thread reads it when sending the next reply). */
void airplay_rtsp_server_set_single_cipher_frame(airplay_rtsp_server_t *server,
                                                 bool enabled);
bool airplay_rtsp_server_single_cipher_frame(const airplay_rtsp_server_t *server);

/* Stops the connection and all endpoints.  Must be called from the poll
 * thread; no callback is invoked afterwards. */
void airplay_rtsp_server_stop(airplay_rtsp_server_t *server);
