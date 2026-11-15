/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airplay_rtsp_pairing.h"
#include "airplay_audio.h"
#include "rtcp/data_stream.h"

/* The audio advertisement (audioFormats/audioLatencies) grows /info to about
 * 4.2 KB; keep this in step with AIRPLAY_INFO_PLIST_MAX_BYTES. */
#define AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES RTCP_CAPABILITIES_MAX_BYTES
#define AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES 8192U

typedef struct airplay_rtsp_session airplay_rtsp_session_t;

typedef bool (*airplay_rtsp_start_screen_cb_t)(
    const airplay_rtsp_pairing_t *pairing,
    uint64_t stream_connection_id,
    uint16_t requested_port,
    uint16_t *bound_port,
    void *context);
typedef bool (*airplay_rtsp_start_event_cb_t)(
    const airplay_rtsp_pairing_t *pairing,
    uint16_t *bound_port,
    void *context);
typedef bool (*airplay_rtsp_start_timing_cb_t)(
    uint16_t peer_port,
    uint16_t *bound_port,
    void *context);
/* audio_format_bits is the sender's SETUP audioFormat value.  A false return
 * rejects the SETUP with 503.  stream_type is 100 (main), 101 (alt) or 102
 * (high). */
typedef bool (*airplay_rtsp_start_audio_cb_t)(
    const airplay_rtsp_pairing_t *pairing,
    uint16_t stream_type,
    uint64_t stream_connection_id,
    uint64_t audio_format_bits,
    uint16_t *data_port,
    uint16_t *control_port,
    void *context);
typedef void (*airplay_rtsp_stop_audio_cb_t)(uint16_t stream_type,
                                             void *context);
typedef void (*airplay_rtsp_stop_media_cb_t)(void *context);

/* SET_PARAMETER "volume: <dB>" from the sender (text/parameters body).
 * 0 dB is full scale and -144 dB is silence; the callback is invoked only
 * when a well-formed volume parameter is present. */
typedef void (*airplay_rtsp_volume_cb_t)(float volume_db, void *context);

typedef bool (*airplay_rtsp_start_iap_cb_t)(const airplay_rtsp_pairing_t *, uint64_t seed,
                                           uint16_t *port, void *context);
typedef bool (*airplay_rtsp_disable_bt_cb_t)(const char *device_id, void *context);
typedef struct {
    airplay_rtsp_pairing_config_t pairing;
    const uint8_t *info_plist;
    size_t info_plist_len;
    uint16_t screen_requested_port;
    airplay_rtsp_start_screen_cb_t start_screen;
    airplay_rtsp_start_event_cb_t start_event;
    airplay_rtsp_start_timing_cb_t start_timing;
    airplay_rtsp_start_audio_cb_t start_audio; /* legacy one stream per type */
    bool (*start_audio_ex)(const airplay_rtsp_pairing_t *, uint16_t,
                           const airplay_audio_options_t *, uint16_t *, uint16_t *, void *);
    void (*stop_audio_ex)(uint16_t, const airplay_audio_options_t *, void *);
    /* Optional second phase for stream role/input; failure rolls back the listener. */
    bool (*configure_audio)(const airplay_rtsp_pairing_t *, uint16_t,
                            const airplay_audio_options_t *, void *);
    airplay_rtsp_stop_media_cb_t stop_screen;
    airplay_rtsp_stop_media_cb_t stop_event;
    airplay_rtsp_stop_media_cb_t stop_timing;
    airplay_rtsp_stop_audio_cb_t stop_audio;
    void *media_context;
    airplay_rtsp_volume_cb_t on_volume;
    void *volume_context;
    airplay_rtsp_start_iap_cb_t start_iap;
    airplay_rtsp_stop_media_cb_t stop_iap;
    airplay_rtsp_disable_bt_cb_t disable_bluetooth;
    void *command_context;
    void (*on_audio_modes)(const airplay_audio_modes_t *, void *context);
    bool (*on_audio_duck)(bool ducked, float volume_db, uint32_t duration_ms, void *context);
    uint32_t data_stream_mask;
    bool negotiate_ui_sync;
    bool negotiate_vehicle;
    bool (*start_data_stream)(const airplay_rtsp_pairing_t *, rtcp_data_stream_kind_t,
                              uint64_t seed, uint32_t stream_id, uint16_t *, void *);
    void (*stop_data_stream)(rtcp_data_stream_kind_t, void *);
    /* Internal experimental multi-video backend. Both callbacks are required.
     * When supplied they own Type 110 and Type 111 independently; the legacy
     * start_screen/stop_screen remain unchanged when absent. This hook alone
     * neither declares a second display nor negotiates altScreen. No public
     * Receiver opt-in, second listener, HEVC or official Ultra is implied.
     * Start must clean its partial resources before returning false. A true
     * result with port zero is refused and followed by stop for that type. */
    bool (*start_video)(const airplay_rtsp_pairing_t *, uint16_t stream_type,
                        uint64_t connection_id, uint16_t requested_port,
                        uint16_t *bound_port, void *);
    void (*stop_video)(uint16_t stream_type, void *);
    uint16_t alt_screen_requested_port;
} airplay_rtsp_session_config_t;

typedef enum {
    AIRPLAY_RTSP_SESSION_OK = 0,
    AIRPLAY_RTSP_SESSION_INVALID_ARGUMENT,
    AIRPLAY_RTSP_SESSION_NO_MEMORY,
    AIRPLAY_RTSP_SESSION_INTERNAL_ERROR,
} airplay_rtsp_session_result_t;

airplay_rtsp_session_result_t airplay_rtsp_session_create(
    const airplay_rtsp_session_config_t *config,
    airplay_rtsp_session_t **out_session);

/* Creates a session that borrows config->info_plist. The bytes must remain
 * immutable and valid until airplay_rtsp_session_destroy(). */
airplay_rtsp_session_result_t airplay_rtsp_session_create_borrowing_info_plist(
    const airplay_rtsp_session_config_t *config,
    airplay_rtsp_session_t **out_session);

/* Handles one parsed RTSP request and writes one complete response. The caller
 * must send the response before switching the RTSP socket to ControlCipher. */
airplay_rtsp_session_result_t airplay_rtsp_session_handle(
    airplay_rtsp_session_t *session,
    const airplay_rtsp_request_t *request,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len,
    bool *enable_control_cipher);

airplay_pair_verify_result_t airplay_rtsp_session_get_control_keys(
    const airplay_rtsp_session_t *session,
    airplay_pair_verify_control_keys_t *out_keys);
void airplay_rtsp_session_mark_control_cipher_active(
    airplay_rtsp_session_t *session);
void airplay_rtsp_session_destroy(airplay_rtsp_session_t *session);
bool airplay_rtsp_session_is_established(const airplay_rtsp_session_t *session);

/* Owner reports terminal socket loss; consumed seed remains non-reusable. */
void airplay_rtsp_session_iap_closed(airplay_rtsp_session_t *session);
void airplay_rtsp_session_data_stream_closed(airplay_rtsp_session_t *, rtcp_data_stream_kind_t);
bool airplay_rtsp_session_data_stream_offered(const airplay_rtsp_session_t *, rtcp_data_stream_kind_t);

bool airplay_rtsp_session_data_stream_enabled(const airplay_rtsp_session_t *, rtcp_data_stream_kind_t);
