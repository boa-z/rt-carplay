/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file iap2_session.h
 * Portable iAP2 link composition. Serialize all calls on one owner.
 * Session callbacks execute synchronously and must not reenter or destroy it. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "iap2_core.h"
#include "iap2_identity.h"
#include "iap2_mfi_auth.h"
#include "rtcp/device_time.h"
#include "rtcp/media.h"
#include "rtcp/mfi.h"
#include "rtcp/navigation.h"
#include "rtcp/phone_status.h"

/*
 * Accessory-side iAP2 negotiation for the wireless (Bluetooth) CarPlay path.
 *
 * The session owns the link layer and the MFi/identification/CarPlay state
 * machine.  Everything platform-specific is injected:
 *   - tx:        RFCOMM/link transport
 *   - mfi:       MFi coprocessor (certificate + challenge signature)
 *   - get_wifi:  current SoftAP credentials
 *   - get_airplay: AirPlay endpoint availability
 */

/** Opaque, owner-thread iAP2 link and negotiation state. */
typedef struct iap2_session iap2_session_t;

typedef struct
{
    const char *ssid;
    const char *passphrase;
    uint8_t channel;
    uint8_t security_type; /* 0=None, 1=WEP, 2=WPA/WPA2, 3=WPA3 transition, 4=WPA3 */
    uint8_t bssid[6];
} iap2_session_wifi_t;

typedef struct
{
    uint16_t airplay_port;
    const char *ip_address;
    const char *device_identifier;
    const char *public_key;
    const char *source_version;
    bool mdns_ready;
    bool event_connected;
    bool transport_connected; /* TCP accepted; stop wired startup retries before SETUP. */
} iap2_session_airplay_t;

typedef enum
{
    IAP2_SESSION_EVENT_IDENTIFICATION_ACCEPTED = 0,
    IAP2_SESSION_EVENT_IDENTIFICATION_REJECTED,
    IAP2_SESSION_EVENT_MFI_CERTIFICATE_SENT,
    IAP2_SESSION_EVENT_MFI_AUTHENTICATED,
    IAP2_SESSION_EVENT_MFI_FAILED,
    IAP2_SESSION_EVENT_SUBSCRIPTIONS_SENT,
    IAP2_SESSION_EVENT_WIFI_CONFIG_SENT,
    IAP2_SESSION_EVENT_CARPLAY_INVITE_SENT,
    IAP2_SESSION_EVENT_CARPLAY_REINVITE_SENT,
    IAP2_SESSION_EVENT_LINK_READY,
    IAP2_SESSION_EVENT_LINK_DEAD,

    /*
     * Specific reasons behind IAP2_SESSION_EVENT_MFI_FAILED.  Each is
     * emitted immediately before the generic MFI_FAILED event, so the
     * umbrella event keeps its meaning for existing consumers while a
     * bring-up log can tell a phone rejection (0xAA04) apart from a local
     * certificate/signature failure.
     */
    IAP2_SESSION_EVENT_MFI_CERT_FAILED,          /* cert read or 0xAA01 send failed */
    IAP2_SESSION_EVENT_MFI_CHALLENGE_INVALID,    /* 0xAA02 body missing/bad length */
    IAP2_SESSION_EVENT_MFI_SIGN_FAILED,          /* coprocessor sign returned an error */
    IAP2_SESSION_EVENT_MFI_STATE_INVALID,        /* 0xAA02/0xAA05 out of sequence */
    IAP2_SESSION_EVENT_MFI_ACK_FAILED,           /* 0xAA03 was not link-acked */
    IAP2_SESSION_EVENT_MFI_PHONE_REJECTED,       /* phone sent 0xAA04 */
    IAP2_SESSION_EVENT_MFI_RESPONSE_SEND_FAILED, /* 0xAA03 could not be queued */
    IAP2_SESSION_EVENT_CARPLAY_AVAILABILITY_RECEIVED,
    IAP2_SESSION_EVENT_CARPLAY_INVITE_ACKED, /* link ACK, not application acceptance */
} iap2_session_event_t;

/*
 * The session below the negotiation layer is a byte stream.  Wireless
 * sessions run it over Bluetooth RFCOMM; wired sessions run it over the
 * USBMUX carkit service stream.  The transport only changes the
 * identification profile, the Wi-Fi exchange and the 0x4301 body; the iAP2
 * link, MFi and message sequencing are shared.
 */
typedef enum
{
    IAP2_SESSION_TRANSPORT_WIRELESS = 0,
    IAP2_SESSION_TRANSPORT_WIRED_USB,
} iap2_session_transport_t;

/** Supply current AP credentials. Keep strings alive for immediate
 * serialization after return. False means the AP is unavailable. */
typedef bool (*iap2_session_get_wifi_cb_t)(iap2_session_wifi_t *out, void *context);
/** Supply the current AirPlay endpoint. False defers an invitation. Keep
 * strings alive until the session copies them immediately after return. */
typedef bool (*iap2_session_get_airplay_cb_t)(iap2_session_airplay_t *out, void *context);
/** Observe a negotiation or link state transition on the session owner. */
typedef void (*iap2_session_event_cb_t)(iap2_session_event_t event, void *context);
/** Optional synchronous observer. Body is borrowed until return; do not retain
 * it or reenter/destroy the session. Outgoing means queued, not peer accepted. */
typedef void (*iap2_session_message_cb_t)(bool outgoing, uint16_t message_id,
                                          const uint8_t *body, size_t body_len,
                                          void *context);

/** iAP2 link providers and optional metadata callbacks. Copy the config at
 * create and retain its referenced providers, contexts and strings. */
typedef struct
{
    iap2_identity_config_t identity; /* strings are borrowed for the session lifetime */
    const struct rtcp_mfi *mfi;      /* NULL disables the MFi exchange */
    iap2_core_tx_cb_t tx;
    iap2_session_get_wifi_cb_t get_wifi; /* wireless only */
    iap2_session_get_airplay_cb_t get_airplay;
    iap2_session_event_cb_t on_event;
    void *context;

    iap2_session_transport_t transport;
    /* Wired only: CarPlay NCM control interface number in the active USB
     * configuration (the references use 3). */
    bool airplay_tunnel; /* reliable Wi-Fi DataStream, independent iAP2 link */
    uint8_t car_play_interface_number;
    /* Wired only: reported to the phone after authentication.  Zero means
     * "no charging current claimed", matching the reference bring-up. */
    uint16_t available_current_ma;
    bool battery_should_charge;
    /* Application compatibility policy, not an iAP2 timing requirement.
     * Zero disables it. One extra wired invitation is allowed before TCP. */
    uint32_t wired_reinvite_delay_ms;
    iap2_session_message_cb_t observe_message;
    rtcp_now_playing_cb_t on_now_playing;
    rtcp_route_guidance_cb_t on_route_guidance;
    rtcp_route_maneuver_cb_t on_route_maneuver;
    rtcp_device_time_cb_t on_device_time;
    /** Phone status on wired and Wi-Fi runtime links. NULL disables delivery. */
    rtcp_phone_status_cb_t on_phone_status;
    rtcp_now_playing_details_cb_t on_now_playing_details;
    rtcp_route_details_cb_t on_route_details;
    /** Requires on_route_details for route invalidations. */
    rtcp_lane_guidance_cb_t on_lane_guidance;
    /** Opt in to a NULL on_device_time update when link clock data expires. */
    bool notify_device_time_invalidation;
    /** When set, takes precedence over the legacy mfi field. */
    const rtcp_mfi_provider_t *mfi_provider;
    /** RFCOMM fallback after sending Wi-Fi configuration. Zero disables it. */
    uint32_t wireless_invite_delay_ms;
} iap2_session_config_t;

#define IAP2_SESSION_IDENTIFICATION_MAX_RETRIES       4U
#define IAP2_SESSION_PRE_TRANSPORT_WIFI_CONFIG_LIMIT  5U
#define IAP2_SESSION_POST_TRANSPORT_WIFI_CONFIG_LIMIT 2U
#define IAP2_SESSION_CARPLAY_REINVITE_INTERVAL_MS     10000U
#define IAP2_SESSION_CARPLAY_REINVITE_MAX             5U

/** Allocate a session and copy config. Providers, identity strings and user
 * context are borrowed through destroy. Returns NULL for invalid input or
 * allocation failure. */
iap2_session_t *iap2_session_create(const iap2_session_config_t *config);
/** Free the session. Call link_down first to deliver invalidations. */
void iap2_session_destroy(iap2_session_t *session);

/** Begin a fresh negotiation on an available transport. Use monotonic
 * milliseconds from the same clock as feed and advance. */
void iap2_session_link_up(iap2_session_t *session, uint64_t now_ms);
/** Drop pending link traffic and invalidate link-scoped data immediately. */
void iap2_session_link_down(iap2_session_t *session);

/** Feed a transport fragment. Bytes are consumed or copied before return.
 * Fragments received after link_down are ignored. */
void iap2_session_feed(iap2_session_t *session, const uint8_t *bytes, size_t len,
                       uint64_t now_ms);

/** Poll retransmission deadlines and invitation pacing with monotonic time. */
void iap2_session_advance(iap2_session_t *session, uint64_t now_ms);

/** Update AirPlay event-channel readiness and invitation pacing. */
void iap2_session_notify_airplay_event(iap2_session_t *session, bool connected,
                                       uint64_t now_ms);

/**
 * Route one already-parsed iAP2 control message.  Exposed for transports that
 * demultiplex messages outside this session (and for unit tests); the normal
 * path calls it from the link layer callback. Body bytes are borrowed until
 * return. A down link ignores the call.
 */
void iap2_session_handle_message(iap2_session_t *session,
                                 uint16_t message_id,
                                 const uint8_t *body,
                                 size_t body_len,
                                 uint64_t now_ms);

/** Return whether MFi authentication completed on this link. */
bool iap2_session_is_authenticated(const iap2_session_t *session);
/** Return whether the phone accepted this link's IdentificationInformation. */
bool iap2_session_identification_accepted(const iap2_session_t *session);

/** Return requested RTCP_LOCATION_* bits after authentication and
 * identification. Stop, link loss and reauthentication clear the mask. */
unsigned iap2_session_location_requested(const iap2_session_t *session);
/** Queue one subscribed, checksummed GGA/RMC sentence without CR/LF. Input
 * is copied before return. False indicates rejection or queue failure. */
bool iap2_session_send_location(iap2_session_t *session, const char *sentence,
                                size_t length, uint64_t now_ms);

/** Owner-thread PASCD report with one sample. sample_ms is monotonic time
 * since host boot. Speed is an unsigned magnitude in millimetres/second.
 * Returns true when queued on a subscribed runtime link. */
bool iap2_session_send_vehicle_speed(iap2_session_t *session, uint64_t sample_ms,
                                     rtcp_vehicle_gear_t gear, uint32_t speed_mm_s,
                                     uint64_t now_ms);
