/* SPDX-License-Identifier: GPL-3.0-or-later */
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

typedef struct rtcp_receiver rtcp_receiver_t;

/* Public host composition. create copies this configuration and initial
 * /info bytes. OS/net tables, identity data and callback contexts remain
 * borrowed and immutable until destroy. No board or protocol-private header
 * is needed to populate this structure. See receiver_compat.h for migration. */
typedef struct
{
    rtcp_platform_t platform;
    rtcp_media_backend_t media;
    rtcp_iap2_backend_t iap2;
    uint16_t port;                /* 0 selects the standard RTSP port (7000). */
    uint16_t screen_port;         /* 0 requests an ephemeral Screen DataStream port. */
    uint8_t family;               /* 0/RTCP_AF_INET or RTCP_AF_INET6. */
    size_t screen_max_body_bytes; /* 0 selects the protocol default. */
    void (*on_event_state)(bool connected, void *context);
    void *event_state_context;
    rtcp_receiver_event_cb_t on_state;
    void *context;
    rtcp_data_stream_backend_t data_stream;
    /* Optional borrowed processor; attach to only one Receiver, retain until
     * destroy, use only on owner. start/stop and authentication loss reset
     * subscriptions/pending events while preserving current vehicle values.
     * Automatically registers both Vehicle lanes, rejects overlapping raw
     * registration, and owns replies/notification retry. Attachment alone does not modify
     * /info or advertise enabledFeatures. NULL preserves the Classic path. */
    rtcp_vehicle_t *vehicle;
    /* Optional borrowed UI Sync processor, exclusive to this Receiver until
     * destroy. Owns UI output/retry; rejects overlapping raw UI registration.
     * Channel close/authentication loss/start/stop resets the handshake and
     * pending output, retaining desired layout/appearance. No /info declaration. */
    rtcp_ui_sync_t *ui_sync;
    /* Explicit Vehicle State negotiation opt-in. Requires vehicle and exact
     * vehicleStateProtocolInfo matching that model in initial/refreshed /info.
     * Invalid initial metadata fails create; invalid refresh rejects the new
     * controller. SETUP enables only when the phone offers the protocol.
     * Does not enable UI Sync, HEVC, altScreen or official Ultra. */
    bool negotiate_vehicle;
    /* Experimental UI Sync opt-in. Requires attached ui_sync, bound actual
     * host inventory and matching single-Main-Display /info metadata. Invalid
     * initial/refresh metadata is refused. SETUP additionally requires the
     * phone's uiSync offer. The host must provide its actual asset/renderer/
     * presentation implementation; neither this flag nor the inventory proves
     * those capabilities or official Ultra. Keep inventory immutable while
     * attached. D213's independent LVGL dashboard does not satisfy this contract. */
    bool negotiate_ui_sync;
    /* Transport bench only: authenticated Type 111 with a separate listener,
     * key/nonce epoch and media.on_alt_video (NULL = intentional Null Sink).
     * Default false. Does not advertise altScreen or a second display, nor
     * negotiate official Ultra. No board renderer/codec is added. */
    bool experimental_alt_video;
    uint16_t alt_screen_port; /* 0 requests an independent ephemeral port. */
} rtcp_receiver_config_t;

/* All functions on an instance belong to one ordinary thread. The host
 * queues cross-thread commands and joins its poll worker before destroy.
 * No internal thread or GUI is required. start/stop are idempotent. stop
 * closes protocol endpoints synchronously: no protocol callback follows it.
 * A stopped instance can be restarted with the same borrowed providers. */
int rtcp_receiver_create(const rtcp_receiver_config_t *, rtcp_receiver_t **);
int rtcp_receiver_start(rtcp_receiver_t *);
int rtcp_receiver_poll(rtcp_receiver_t *, int timeout_ms);
int rtcp_receiver_stop(rtcp_receiver_t *);
int rtcp_receiver_destroy(rtcp_receiver_t *);
int rtcp_receiver_get_state(const rtcp_receiver_t *, rtcp_receiver_state_t *);
uint16_t rtcp_receiver_port(const rtcp_receiver_t *);
/* Owner-thread observations, including callbacks on that thread. get_peer
 * clears the output and returns ENOENT when no client is attached. Diagnostics
 * returns zero counters for a stopped Receiver; output is copied, never borrowed. */
int rtcp_receiver_get_peer(const rtcp_receiver_t *, rtcp_addr_t *);
int rtcp_receiver_get_diagnostics(const rtcp_receiver_t *,
                                  rtcp_receiver_diagnostics_t *);
/* Separate from Main video_ready and Main diagnostics. Works while stopped. */
int rtcp_receiver_get_alt_video_status(const rtcp_receiver_t *, rtcp_alt_video_status_t *);
/* Owner-thread A/B diagnostic. Default chunked ControlCipher framing is
 * unchanged. Requires a running Receiver; stop/start resets this setting. */
int rtcp_receiver_set_single_cipher_frame(rtcp_receiver_t *, bool enabled);
/* Owner-thread tunneled iAP2 output, including iAP2 callbacks during poll.
 * Bytes are consumed synchronously. False when the event channel is absent. */
bool rtcp_receiver_send_iap(rtcp_receiver_t *, const uint8_t *, size_t);

/* Owner-thread Classic appearance update for the negotiated main display.
 * In Automatic, the host supplies current UI/map night decisions from its
 * sensors or vehicle signals. Light/Dark override those decisions. Returns
 * ESTATE until the encrypted event channel is ready, ENOTSUP without a
 * main display UUID, and EIO if either command could not be written.
 * Success reports socket writes. The host keeps the preference and can
 * reapply it after reconnect or supply it in the next /info payload. */
int rtcp_receiver_set_appearance(rtcp_receiver_t *, rtcp_appearance_setting_t,
                                 bool ui_night, bool map_night);

/* Publish the host's applied gain for phone GET_PARAMETER volume requests.
 * Call on the owner, including from on_volume after applying the gain.
 * Accepts finite values in [-144, 0] dB, with -144 representing mute.
 * The value survives stop/start. The setter does not change host gain or
 * send an unsolicited message. Until called, volume queries are empty. */
int rtcp_receiver_set_volume_db(rtcp_receiver_t *, float volume_db);

/* Optional owner-thread callback for Classic AirPlay Now Playing updates.
 * NULL disables delivery. Registration survives stop/start. The host clears
 * its metadata on session loss using the Receiver state callback. */
int rtcp_receiver_set_now_playing_callback(rtcp_receiver_t *, rtcp_now_playing_cb_t, void *context);

/* Owner-thread Type 130 transport, including on_body callbacks. send copies
 * bytes into a bounded queue; success is not phone receipt. EINVAL denotes
 * invalid kind/body, ESTATE an absent connection, ENOMEM a full queue.
 * Status works while stopped (zero counters/idle, registration preserved).
 * Sending to attached Vehicle State/UI Sync lanes returns ESTATE; update
 * their models through the corresponding owner-thread APIs instead. */
int rtcp_receiver_send_data_stream(rtcp_receiver_t *, rtcp_data_stream_kind_t,
                                   const uint8_t *, size_t);
int rtcp_receiver_get_data_stream_status(const rtcp_receiver_t *,
                                         rtcp_data_stream_kind_t, rtcp_data_stream_status_t *);
int rtcp_receiver_get_vehicle_delivery_status(const rtcp_receiver_t *,
                                              rtcp_vehicle_delivery_status_t *);
int rtcp_receiver_get_ui_sync_delivery_status(const rtcp_receiver_t *,
                                              rtcp_ui_sync_delivery_status_t *);

#ifdef __cplusplus
}
#endif
#endif
