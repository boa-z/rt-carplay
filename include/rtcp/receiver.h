/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file receiver.h
 * Public Receiver composition and single-owner session API. */
#ifndef RTCP_RECEIVER_H
#define RTCP_RECEIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "rtcp/rtcp.h"
#include "rtcp/platform.h"
#include "rtcp/media.h"
#include "rtcp/events.h"
#include "rtcp/input.h"
#include "rtcp/diagnostics.h"
#include "rtcp/data_stream.h"
#include "rtcp/display.h"
#include "rtcp/vehicle.h"
#include "rtcp/ui_sync.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Opaque Receiver. Use only on the thread that owns its lifecycle and poll. */
typedef struct rtcp_receiver rtcp_receiver_t;

/** Public host composition. create copies this configuration and initial
 * /info bytes. OS/net tables, identity data and callback contexts remain
 * borrowed and immutable until destroy. No board or protocol-private header
 * is needed to populate this structure. See receiver_compat.h for migration. */
typedef struct
{
    rtcp_platform_t platform;     /**< Borrowed platform services and identity providers. */
    rtcp_media_backend_t media;   /**< Synchronous video, audio and input backends. */
    rtcp_iap2_backend_t iap2;     /**< Optional wireless runtime tunnel owner. */
    uint16_t port;                /**< 0 selects the standard RTSP port (7000). */
    uint16_t screen_port;         /**< 0 requests an ephemeral Screen DataStream port. */
    uint8_t family;               /**< 0/RTCP_AF_INET or RTCP_AF_INET6. */
    size_t screen_max_body_bytes; /**< 0 selects the protocol default. */
    /** Encrypted event-channel readiness on the poll owner. */
    void (*on_event_state)(bool connected, void *context);
    void *event_state_context;
    rtcp_receiver_event_cb_t on_state;
    void *context;
    rtcp_data_stream_backend_t data_stream;
    /** Optional borrowed processor; attach to only one Receiver, retain until
     * destroy, use only on owner. start/stop and authentication loss reset
     * subscriptions/pending events while preserving current vehicle values.
     * Automatically registers both Vehicle lanes, rejects overlapping raw
     * registration, and owns replies/notification retry. Attachment alone does not modify
     * /info or advertise enabledFeatures. NULL preserves the Classic path. */
    rtcp_vehicle_t *vehicle;
    /** Optional borrowed UI Sync processor, exclusive to this Receiver until
     * destroy. Owns UI output/retry; rejects overlapping raw UI registration.
     * Channel close/authentication loss/start/stop resets the handshake and
     * pending output, retaining desired layout/appearance. No /info declaration. */
    rtcp_ui_sync_t *ui_sync;
    /** Explicit Vehicle State negotiation opt-in. Requires vehicle and exact
     * vehicleStateProtocolInfo matching that model in initial/refreshed /info.
     * Invalid initial metadata fails create; invalid refresh rejects the new
     * controller. SETUP enables only when the phone offers the protocol.
     * Does not enable UI Sync, HEVC, altScreen or official Ultra. */
    bool negotiate_vehicle;
    /** Experimental UI Sync opt-in. Requires attached ui_sync, bound actual
     * host inventory and matching single-Main-Display /info metadata. Invalid
     * initial/refresh metadata is refused. SETUP additionally requires the
     * phone's uiSync offer. The host must provide its actual asset/renderer/
     * presentation implementation; neither this flag nor the inventory proves
     * those capabilities or official Ultra. Keep inventory immutable while
     * attached. D213's independent LVGL dashboard does not satisfy this contract. */
    bool negotiate_ui_sync;
    /** Transport bench only: authenticated Type 111 with a separate listener,
     * key/nonce epoch and media.on_alt_video (NULL = intentional Null Sink).
     * Default false. Does not advertise altScreen or a second display, nor
     * negotiate official Ultra. No board renderer/codec is added. */
    bool experimental_alt_video;
    uint16_t alt_screen_port; /* 0 requests an independent ephemeral port. */
} rtcp_receiver_config_t;

/** Create a stopped Receiver. Returns RTCP_OK and transfers instance
 * ownership to the caller, RTCP_EINVAL for invalid configuration, or
 * RTCP_ENOMEM if allocation fails.
 *
 * All functions on an instance belong to one ordinary thread. The host
 * queues cross-thread commands and joins its poll worker before destroy.
 * No internal thread or GUI is required. start/stop are idempotent. stop
 * closes protocol endpoints synchronously: no protocol callback follows it.
 * A stopped instance can be restarted with the same borrowed providers. */
int rtcp_receiver_create(const rtcp_receiver_config_t *, rtcp_receiver_t **);
/** Open the listener and initialize the media endpoints. Idempotent when
 * running. Returns RTCP_OK or the negative startup error after rollback. */
int rtcp_receiver_start(rtcp_receiver_t *);
/** Service connections and dispatch callbacks synchronously. timeout_ms is
 * the maximum wait in milliseconds, with 0 nonblocking. Returns RTCP_OK
 * or a negative RTCP error, including RTCP_ESTATE when stopped. */
int rtcp_receiver_poll(rtcp_receiver_t *, int timeout_ms);
/** Close session resources synchronously. Idempotent when stopped.
 * Returns RTCP_OK or RTCP_ESTATE if called during lifecycle notification. */
int rtcp_receiver_stop(rtcp_receiver_t *);
/** Stop and free the owned instance. A failed stop leaves ownership with
 * the caller. The pointer is invalid after RTCP_OK. */
int rtcp_receiver_destroy(rtcp_receiver_t *);
/** Copy the current state, including while stopped. Returns RTCP_EINVAL
 * for a NULL instance or output. Safe from the owner state callback. */
int rtcp_receiver_get_state(const rtcp_receiver_t *, rtcp_receiver_state_t *);
/** Return the bound RTSP port, or zero when stopped or absent. */
uint16_t rtcp_receiver_port(const rtcp_receiver_t *);
/** Owner-thread observation, including callbacks on that thread. get_peer
 * clears the output and returns ENOENT when no client is attached. Diagnostics
 * returns zero counters for a stopped Receiver; output is copied, never borrowed. */
int rtcp_receiver_get_peer(const rtcp_receiver_t *, rtcp_addr_t *);
/** Copy current diagnostics or zero counters while stopped. Returns
 * RTCP_EINVAL for a NULL instance or output. */
int rtcp_receiver_get_diagnostics(const rtcp_receiver_t *,
                                  rtcp_receiver_diagnostics_t *);
/** Copy Type 111 transport diagnostics, independent of Main video readiness.
 * Works while stopped. Returns RTCP_EINVAL for a NULL instance or output. */
int rtcp_receiver_get_alt_video_status(const rtcp_receiver_t *, rtcp_alt_video_status_t *);
/** Owner-thread A/B diagnostic. Default chunked ControlCipher framing is
 * unchanged. Requires a running Receiver; stop/start resets this setting. */
int rtcp_receiver_set_single_cipher_frame(rtcp_receiver_t *, bool enabled);
/** Owner-thread tunneled iAP2 output, including iAP2 callbacks during poll.
 * Bytes are consumed synchronously. False when the event channel is absent. */
bool rtcp_receiver_send_iap(rtcp_receiver_t *, const uint8_t *, size_t);

/** Last known owner of the Classic MainScreen resource. */
typedef enum
{
    RTCP_PROJECTION_UNKNOWN,
    RTCP_PROJECTION_PHONE,
    RTCP_PROJECTION_HOST,
} rtcp_projection_owner_t;

/** Owner-thread MainScreen focus request with the Classic session kept open.
 * false requests the host screen; true asks iPhone to foreground CarPlay.
 * OK means the command was written. Confirm ownership with the getter and
 * wait for video before presenting resumed frames. Audio and iAP2 are not
 * addressed by these commands. ESTATE if the session/event channel is absent. */
int rtcp_receiver_set_projection_enabled(rtcp_receiver_t *, bool enabled);
/** Return current MainScreen ownership, or UNKNOWN when unavailable. */
rtcp_projection_owner_t rtcp_receiver_get_projection_owner(const rtcp_receiver_t *);

/** Owner-thread Type 110 identity for the lifetime of one started Receiver.
 * Zero means no active stream. A replacement stream gets a new identity. */
uint32_t rtcp_receiver_get_projection_stream_generation(const rtcp_receiver_t *);

/** Request a fresh Classic MainScreen keyframe from the Receiver owner thread.
 * @param receiver Running Receiver. The call requires an active Screen stream and event channel.
 * @return True when sent or coalesced by the session rate limit. False if unavailable or sending failed.
 * Confirm recovery by consuming a decodable keyframe. */
bool rtcp_receiver_request_screen_keyframe(rtcp_receiver_t *receiver);

/** Optional callback for the phone's plain requestUI (for example from the
 * OEM tile). Called on the Receiver owner; true accepts the request. The
 * host owns its local display and calls the projection API as appropriate.
 * Registration survives stop/start; NULL clears it. */
typedef bool (*rtcp_host_ui_cb_t)(void *context);
/** Register a host UI request callback. Borrow context until replaced or
 * destroy. Pass NULL to clear. Returns RTCP_EINVAL for a NULL instance. */
int rtcp_receiver_set_host_ui_callback(rtcp_receiver_t *, rtcp_host_ui_cb_t, void *context);

/** Owner-thread Classic appearance update for the negotiated main display.
 * In Automatic, the host supplies current UI/map night decisions from its
 * sensors or vehicle signals. Light/Dark override those decisions. Returns
 * ESTATE until the encrypted event channel is ready, ENOTSUP without a
 * main display UUID, and EIO if either command could not be written.
 * Success reports socket writes. The host keeps the preference and can
 * reapply it after reconnect or supply it in the next /info payload. */
int rtcp_receiver_set_appearance(rtcp_receiver_t *, rtcp_appearance_setting_t,
                                 bool ui_night, bool map_night);

/** Publish the host's applied gain for phone GET_PARAMETER volume requests.
 * Call on the owner, including from on_volume after applying the gain.
 * Accepts finite values in [-144, 0] dB, with -144 representing mute.
 * The value survives stop/start. The setter does not change host gain or
 * send an unsolicited message. Until called, volume queries are empty. */
int rtcp_receiver_set_volume_db(rtcp_receiver_t *, float volume_db);

/** Optional owner-thread callback for Classic AirPlay Now Playing updates.
 * NULL disables delivery. Registration survives stop/start. The host clears
 * its metadata on session loss using the Receiver state callback. */
int rtcp_receiver_set_now_playing_callback(rtcp_receiver_t *, rtcp_now_playing_cb_t, void *context);

/** Optional AirPlay details callback on the Receiver owner. Registration
 * survives stop/start. An invalidation retires the last session's metadata.
 * NULL disables delivery. Returns RTCP_EINVAL for a NULL Receiver. */
int rtcp_receiver_set_now_playing_details_callback(rtcp_receiver_t *, rtcp_now_playing_details_cb_t, void *context);

/** Owner-thread Type 130 transport, including on_body callbacks. send copies
 * bytes into a bounded queue; success is not phone receipt. EINVAL denotes
 * invalid kind/body, ESTATE an absent connection, ENOMEM a full queue.
 * Status works while stopped (zero counters/idle, registration preserved).
 * Sending to attached Vehicle State/UI Sync lanes returns ESTATE; update
 * their models through the corresponding owner-thread APIs instead. */
int rtcp_receiver_send_data_stream(rtcp_receiver_t *, rtcp_data_stream_kind_t,
                                   const uint8_t *, size_t);
/** Copy delivery counters and connection state for one Type 130 lane.
 * Returns RTCP_EINVAL for an invalid lane or NULL instance/output. */
int rtcp_receiver_get_data_stream_status(const rtcp_receiver_t *,
                                         rtcp_data_stream_kind_t, rtcp_data_stream_status_t *);
/** Copy Vehicle State delivery status, including attachment and retry
 * counters. Returns RTCP_EINVAL for a NULL instance or output. */
int rtcp_receiver_get_vehicle_delivery_status(const rtcp_receiver_t *,
                                              rtcp_vehicle_delivery_status_t *);
/** Copy UI Sync delivery status, including the last protocol refusal.
 * Returns RTCP_EINVAL for a NULL instance or output. */
int rtcp_receiver_get_ui_sync_delivery_status(const rtcp_receiver_t *,
                                              rtcp_ui_sync_delivery_status_t *);

#ifdef __cplusplus
}
#endif
#endif
