/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "iap2_core.h"
#include "iap2_identity.h"
#include "iap2_mfi_auth.h"
#include "rtcp/mfi.h"

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

typedef struct iap2_session iap2_session_t;

typedef struct {
    const char *ssid;
    const char *passphrase;
    uint8_t channel;
    uint8_t security_type; /* 0=None, 1=WEP, 2=WPA/WPA2, 3=WPA3 transition, 4=WPA3 */
    uint8_t bssid[6];
} iap2_session_wifi_t;

typedef struct {
    uint16_t airplay_port;
    const char *ip_address;
    const char *device_identifier;
    const char *public_key;
    const char *source_version;
    bool mdns_ready;
    bool event_connected;
} iap2_session_airplay_t;

typedef enum {
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
    IAP2_SESSION_EVENT_MFI_CERT_FAILED,       /* cert read or 0xAA01 send failed */
    IAP2_SESSION_EVENT_MFI_CHALLENGE_INVALID, /* 0xAA02 body missing/bad length */
    IAP2_SESSION_EVENT_MFI_SIGN_FAILED,       /* coprocessor sign returned an error */
    IAP2_SESSION_EVENT_MFI_STATE_INVALID,     /* 0xAA02/0xAA05 out of sequence */
    IAP2_SESSION_EVENT_MFI_ACK_FAILED,        /* 0xAA03 was not link-acked */
    IAP2_SESSION_EVENT_MFI_PHONE_REJECTED,    /* phone sent 0xAA04 */
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
typedef enum {
    IAP2_SESSION_TRANSPORT_WIRELESS = 0,
    IAP2_SESSION_TRANSPORT_WIRED_USB,
} iap2_session_transport_t;

typedef bool (*iap2_session_get_wifi_cb_t)(iap2_session_wifi_t *out, void *context);
typedef bool (*iap2_session_get_airplay_cb_t)(iap2_session_airplay_t *out, void *context);
typedef void (*iap2_session_event_cb_t)(iap2_session_event_t event, void *context);

typedef struct {
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
    uint8_t car_play_interface_number;
    /* Wired only: reported to the phone after authentication.  Zero means
     * "no charging current claimed", matching the reference bring-up. */
    uint16_t available_current_ma;
    bool battery_should_charge;
} iap2_session_config_t;

#define IAP2_SESSION_IDENTIFICATION_MAX_RETRIES 4U
#define IAP2_SESSION_PRE_TRANSPORT_WIFI_CONFIG_LIMIT 5U
#define IAP2_SESSION_POST_TRANSPORT_WIFI_CONFIG_LIMIT 2U
#define IAP2_SESSION_CARPLAY_REINVITE_INTERVAL_MS 10000U
#define IAP2_SESSION_CARPLAY_REINVITE_MAX 5U

iap2_session_t *iap2_session_create(const iap2_session_config_t *config);
void iap2_session_destroy(iap2_session_t *session);

/* Call when the RFCOMM link comes up / goes down. */
void iap2_session_link_up(iap2_session_t *session, uint64_t now_ms);
void iap2_session_link_down(iap2_session_t *session);

/* Feed bytes received from the RFCOMM transport. */
void iap2_session_feed(iap2_session_t *session, const uint8_t *bytes, size_t len,
                       uint64_t now_ms);

/* Periodic work: retransmits and CarPlay invite pacing. */
void iap2_session_advance(iap2_session_t *session, uint64_t now_ms);

/* AirPlay event stream state, used to pace CarPlay re-invites. */
void iap2_session_notify_airplay_event(iap2_session_t *session, bool connected,
                                       uint64_t now_ms);

/*
 * Route one already-parsed iAP2 control message.  Exposed for transports that
 * demultiplex messages outside this session (and for unit tests); the normal
 * path calls it from the link layer callback.
 */
void iap2_session_handle_message(iap2_session_t *session,
                                 uint16_t message_id,
                                 const uint8_t *body,
                                 size_t body_len,
                                 uint64_t now_ms);

bool iap2_session_is_authenticated(const iap2_session_t *session);
bool iap2_session_identification_accepted(const iap2_session_t *session);
