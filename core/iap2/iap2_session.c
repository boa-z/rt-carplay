/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Accessory-side wireless iAP2 negotiation state machine.
 *
 * The message flow follows esp32-carplay-web-relay main/app_main.c
 * (GPL-3.0-or-later); platform services are injected through
 * iap2_session_config_t instead of ESP-IDF globals.
 */
#include "iap2_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iap2_carplay.h"
#include "iap2_parameter.h"
#include "rtcp/rtcp.h"

#define IAP2_SESSION_CERT_CAPACITY 1536U
#define IAP2_SESSION_SIGNATURE_CAPACITY 512U
#define IAP2_SESSION_IDENTIFICATION_CAPACITY 1024U
#define IAP2_SESSION_WIFI_BODY_CAPACITY 256U
#define IAP2_SESSION_START_BODY_CAPACITY 1600U
#define IAP2_SESSION_MAX_CHALLENGE_BYTES 128U

#define IAP2_MSG_START_IDENTIFICATION 0x1d00U
#define IAP2_MSG_IDENTIFICATION_INFORMATION 0x1d01U
#define IAP2_MSG_IDENTIFICATION_ACCEPTED 0x1d02U
#define IAP2_MSG_IDENTIFICATION_REJECTED 0x1d03U
#define IAP2_MSG_MFI_AUTH_REQUEST 0xaa00U
#define IAP2_MSG_MFI_CERTIFICATE 0xaa01U
#define IAP2_MSG_MFI_CHALLENGE 0xaa02U
#define IAP2_MSG_MFI_CHALLENGE_RESPONSE 0xaa03U
#define IAP2_MSG_MFI_AUTH_FAILED 0xaa04U
#define IAP2_MSG_MFI_AUTH_SUCCEEDED 0xaa05U
#define IAP2_MSG_CARPLAY_AVAILABILITY 0x4300U
#define IAP2_MSG_CARPLAY_START_SESSION 0x4301U
#define IAP2_MSG_TRANSPORT_NOTIFICATION 0x4e0eU
#define IAP2_MSG_WIFI_CONFIG_REQUEST 0x5702U
#define IAP2_MSG_WIFI_CONFIGURATION 0x5703U

struct iap2_session {
    iap2_session_config_t config;
    iap2_core_t *core;
    iap2_mfi_auth_t auth;

    uint16_t omitted_message_ids[IAP2_IDENTITY_MAX_OMITTED_MESSAGES];
    size_t omitted_message_count;
    unsigned identification_retries;

    bool link_up;
    bool identification_accepted;
    bool subscriptions_sent;
    bool start_session_sent;
    bool transport_notification_seen;
    bool mfi_response_pending;
    bool airplay_event_connected;
    bool carplay_invite_active;
    uint64_t now_ms;
    uint64_t carplay_invite_at_ms;
    unsigned carplay_reinvite_count;
    uint8_t pre_transport_wifi_configs;
    uint8_t post_transport_wifi_configs;
};

static void emit_event(iap2_session_t *session, iap2_session_event_t event)
{
    if (session->config.on_event != NULL) {
        session->config.on_event(event, session->config.context);
    }
}

/* Emit the specific reason first, then the umbrella MFI_FAILED event so an
 * existing consumer that only knows the generic event still sees it. */
static void emit_mfi_failure(iap2_session_t *session, iap2_session_event_t reason)
{
    emit_event(session, reason);
    emit_event(session, IAP2_SESSION_EVENT_MFI_FAILED);
}

static bool send_csm(iap2_session_t *session,
                     uint16_t message_id,
                     const uint8_t *body,
                     size_t body_len,
                     iap2_core_csm_acked_cb_t acked_callback,
                     void *acked_context,
                     uint64_t now_ms)
{
    if (session->core == NULL) return false;
    return iap2_core_send_csm_with_ack_callback(session->core, message_id, body,
                                                body_len, acked_callback,
                                                acked_context, now_ms);
}

static void reset_wireless_negotiation(iap2_session_t *session)
{
    session->start_session_sent = false;
    session->subscriptions_sent = false;
    session->transport_notification_seen = false;
    session->pre_transport_wifi_configs = 0U;
    session->post_transport_wifi_configs = 0U;
    /*
     * airplay_event_connected and the invite pacing are deliberately kept:
     * the phone re-runs MFi/identification mid-session when the Bluetooth
     * path comes up beside an established Wi-Fi AirPlay session.
     */
}

static void reset_identification(iap2_session_t *session)
{
    memset(session->omitted_message_ids, 0, sizeof(session->omitted_message_ids));
    session->omitted_message_count = 0U;
    session->identification_retries = 0U;
    session->identification_accepted = false;
}

static void reset_connection_state(iap2_session_t *session)
{
    iap2_mfi_auth_reset(&session->auth);
    session->mfi_response_pending = false;
    reset_identification(session);
    reset_wireless_negotiation(session);
}

static bool message_is_omitted(const iap2_session_t *session, uint16_t message_id)
{
    for (size_t i = 0; i < session->omitted_message_count; ++i) {
        if (session->omitted_message_ids[i] == message_id) return true;
    }
    return false;
}

static bool send_identification_information(iap2_session_t *session, uint64_t now_ms)
{
    const iap2_identity_config_t *identity = &session->config.identity;
    if (identity->wifi_ssid == NULL || identity->wifi_ssid[0] == '\0') return false;

    uint8_t body[IAP2_SESSION_IDENTIFICATION_CAPACITY];
    size_t body_len = 0U;
    if (!iap2_identity_build_wireless_omitting(identity,
                                               session->omitted_message_ids,
                                               session->omitted_message_count,
                                               body, sizeof(body), &body_len)) {
        return false;
    }
    return send_csm(session, IAP2_MSG_IDENTIFICATION_INFORMATION, body, body_len,
                    NULL, NULL, now_ms);
}

static void handle_identification_rejected(iap2_session_t *session,
                                           const uint8_t *body,
                                           size_t body_len,
                                           uint64_t now_ms)
{
    uint16_t rejected[IAP2_IDENTITY_MAX_OMITTED_MESSAGES];
    size_t rejected_count = 0U;
    if (!iap2_identity_parse_unsupported_messages(body, body_len, rejected,
                                                  IAP2_IDENTITY_MAX_OMITTED_MESSAGES,
                                                  &rejected_count)) {
        emit_event(session, IAP2_SESSION_EVENT_IDENTIFICATION_REJECTED);
        return;
    }

    size_t added = 0U;
    for (size_t i = 0; i < rejected_count; ++i) {
        uint16_t id = rejected[i];
        if (message_is_omitted(session, id)) continue;
        if (session->omitted_message_count >= IAP2_IDENTITY_MAX_OMITTED_MESSAGES) {
            emit_event(session, IAP2_SESSION_EVENT_IDENTIFICATION_REJECTED);
            return;
        }
        session->omitted_message_ids[session->omitted_message_count++] = id;
        ++added;
    }
    if (added == 0U ||
        session->identification_retries >= IAP2_SESSION_IDENTIFICATION_MAX_RETRIES) {
        emit_event(session, IAP2_SESSION_EVENT_IDENTIFICATION_REJECTED);
        return;
    }
    ++session->identification_retries;
    (void)send_identification_information(session, now_ms);
}

static bool read_certificate(iap2_session_t *session,
                             uint8_t *certificate,
                             size_t *certificate_len)
{
    const struct rtcp_mfi *mfi = session->config.mfi;
    if (mfi == NULL || mfi->read_cert == NULL) return false;
    *certificate_len = IAP2_SESSION_CERT_CAPACITY;
    return mfi->read_cert(certificate, IAP2_SESSION_CERT_CAPACITY, certificate_len) ==
               RTCP_OK &&
           *certificate_len != 0U && *certificate_len <= IAP2_SESSION_CERT_CAPACITY;
}

static void handle_authentication_certificate(iap2_session_t *session,
                                              uint64_t now_ms)
{
    uint8_t certificate[IAP2_SESSION_CERT_CAPACITY];
    size_t certificate_len = 0U;
    if (!read_certificate(session, certificate, &certificate_len)) {
        iap2_mfi_auth_mark_failed(&session->auth);
        emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_CERT_FAILED);
        return;
    }

    uint8_t body[IAP2_SESSION_CERT_CAPACITY + 4U];
    size_t body_len = iap2_param_encode(body, sizeof(body), 0,
                                        certificate, certificate_len);
    if (body_len == 0U) {
        iap2_mfi_auth_mark_failed(&session->auth);
        emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_CERT_FAILED);
        return;
    }
    if (send_csm(session, IAP2_MSG_MFI_CERTIFICATE, body, body_len,
                 NULL, NULL, now_ms)) {
        emit_event(session, IAP2_SESSION_EVENT_MFI_CERTIFICATE_SENT);
    } else {
        iap2_mfi_auth_mark_failed(&session->auth);
        emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_CERT_FAILED);
    }
}

static void challenge_response_acked(uint16_t message_id, void *context)
{
    iap2_session_t *session = context;
    if (session == NULL) return;
    session->mfi_response_pending = false;
    if (message_id != IAP2_MSG_MFI_CHALLENGE_RESPONSE ||
        !iap2_mfi_auth_mark_challenge_response_acked(&session->auth)) {
        iap2_mfi_auth_mark_failed(&session->auth);
        emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_ACK_FAILED);
    }
}

static bool handle_authentication_challenge(iap2_session_t *session,
                                            const uint8_t *body,
                                            size_t body_len,
                                            uint64_t now_ms)
{
    const struct rtcp_mfi *mfi = session->config.mfi;
    const uint8_t *challenge = NULL;
    size_t challenge_len = 0U;
    if (mfi == NULL || mfi->sign == NULL ||
        !iap2_param_find_single(body, body_len, 0, &challenge, &challenge_len) ||
        challenge_len == 0U || challenge_len > IAP2_SESSION_MAX_CHALLENGE_BYTES) {
        emit_event(session, IAP2_SESSION_EVENT_MFI_CHALLENGE_INVALID);
        return false;
    }

    uint8_t signature[IAP2_SESSION_SIGNATURE_CAPACITY];
    size_t signature_len = sizeof(signature);
    if (mfi->sign(challenge, challenge_len, signature, sizeof(signature),
                  &signature_len) != RTCP_OK ||
        signature_len == 0U || signature_len > sizeof(signature)) {
        emit_event(session, IAP2_SESSION_EVENT_MFI_SIGN_FAILED);
        return false;
    }

    uint8_t response[IAP2_SESSION_SIGNATURE_CAPACITY + 4U];
    size_t response_len = iap2_param_encode(response, sizeof(response), 0,
                                            signature, signature_len);
    if (response_len != 0U &&
        send_csm(session, IAP2_MSG_MFI_CHALLENGE_RESPONSE, response,
                 response_len, challenge_response_acked, session, now_ms)) {
        return true;
    }
    emit_event(session, IAP2_SESSION_EVENT_MFI_RESPONSE_SEND_FAILED);
    return false;
}

static void send_wireless_subscriptions(iap2_session_t *session, uint64_t now_ms)
{
    uint8_t body[64];
    size_t body_len = 0U;

    static const uint16_t now_playing_fields[] = {1, 4, 6, 12, 26};
    static const uint16_t playback_fields[] = {0, 1, 7};
    uint8_t group0[32];
    uint8_t group1[16];
    size_t group0_len = 0U;
    size_t group1_len = 0U;
    for (size_t i = 0; i < sizeof(now_playing_fields) / sizeof(now_playing_fields[0]); ++i) {
        (void)iap2_param_append_void(group0, sizeof(group0), &group0_len,
                                     now_playing_fields[i]);
    }
    for (size_t i = 0; i < sizeof(playback_fields) / sizeof(playback_fields[0]); ++i) {
        (void)iap2_param_append_void(group1, sizeof(group1), &group1_len,
                                     playback_fields[i]);
    }
    (void)iap2_param_append(body, sizeof(body), &body_len, 0, group0, group0_len);
    (void)iap2_param_append(body, sizeof(body), &body_len, 1, group1, group1_len);
    if (!message_is_omitted(session, 0x5000U)) {
        (void)send_csm(session, 0x5000U, body, body_len, NULL, NULL, now_ms);
    }

    body_len = 0U;
    static const uint8_t route_id[2] = {0, 42};
    (void)iap2_param_append(body, sizeof(body), &body_len, 0, route_id, sizeof(route_id));
    (void)iap2_param_append_void(body, sizeof(body), &body_len, 1);
    (void)iap2_param_append_void(body, sizeof(body), &body_len, 2);
    if (!message_is_omitted(session, 0x5200U)) {
        (void)send_csm(session, 0x5200U, body, body_len, NULL, NULL, now_ms);
    }

    static const uint16_t message_ids[] = {0xae00U, 0x4157U, 0x4154U};
    static const uint8_t parameter_ids[][6] = {
        {4, 5, 6, 0, 0, 0},
        {0, 4, 5, 0, 0, 0},
        {0, 1, 2, 3, 4, 11},
    };
    static const uint8_t parameter_counts[] = {3, 3, 6};
    for (size_t i = 0; i < sizeof(message_ids) / sizeof(message_ids[0]); ++i) {
        if (message_is_omitted(session, message_ids[i])) continue;
        body_len = 0U;
        for (size_t j = 0; j < parameter_counts[i]; ++j) {
            (void)iap2_param_append_void(body, sizeof(body), &body_len,
                                         parameter_ids[i][j]);
        }
        (void)send_csm(session, message_ids[i], body, body_len, NULL, NULL, now_ms);
    }
}

static void maybe_send_wireless_subscriptions(iap2_session_t *session,
                                              uint64_t now_ms)
{
    if (session->subscriptions_sent || !session->identification_accepted ||
        !iap2_mfi_auth_is_authenticated(&session->auth)) {
        return;
    }
    send_wireless_subscriptions(session, now_ms);
    session->subscriptions_sent = true;
    emit_event(session, IAP2_SESSION_EVENT_SUBSCRIPTIONS_SENT);
}

static bool get_wifi(iap2_session_t *session, iap2_session_wifi_t *wifi)
{
    memset(wifi, 0, sizeof(*wifi));
    if (session->config.get_wifi == NULL ||
        !session->config.get_wifi(wifi, session->config.context)) {
        return false;
    }
    return wifi->ssid != NULL && wifi->ssid[0] != '\0' &&
           wifi->passphrase != NULL;
}

static bool send_wifi_configuration(iap2_session_t *session,
                                    bool post_transport,
                                    uint64_t now_ms)
{
    if (!iap2_mfi_auth_is_authenticated(&session->auth)) return false;

    uint8_t *count = post_transport ? &session->post_transport_wifi_configs
                                    : &session->pre_transport_wifi_configs;
    uint8_t limit = post_transport ? IAP2_SESSION_POST_TRANSPORT_WIFI_CONFIG_LIMIT
                                   : IAP2_SESSION_PRE_TRANSPORT_WIFI_CONFIG_LIMIT;
    if (*count >= limit) return false;

    iap2_session_wifi_t wifi;
    if (!get_wifi(session, &wifi)) return false;

    iap2_carplay_wifi_config_t config = {
        .ssid = wifi.ssid,
        .passphrase = wifi.passphrase,
        .channel = wifi.channel,
        .security_type = wifi.security_type,
        .bssid = wifi.bssid,
    };
    uint8_t body[IAP2_SESSION_WIFI_BODY_CAPACITY];
    size_t body_len = 0U;
    if (!iap2_carplay_build_wifi_configuration(&config, body, sizeof(body),
                                               &body_len)) {
        return false;
    }
    if (!send_csm(session, IAP2_MSG_WIFI_CONFIGURATION, body, body_len,
                  NULL, NULL, now_ms)) {
        return false;
    }
    ++*count;
    emit_event(session, IAP2_SESSION_EVENT_WIFI_CONFIG_SENT);
    return true;
}

static bool send_carplay_start_session(iap2_session_t *session, uint64_t now_ms,
                                       bool reinvite)
{
    if (!iap2_mfi_auth_is_authenticated(&session->auth)) return false;
    if (session->start_session_sent) return false;

    iap2_session_airplay_t airplay;
    memset(&airplay, 0, sizeof(airplay));
    if (session->config.get_airplay == NULL ||
        !session->config.get_airplay(&airplay, session->config.context) ||
        !airplay.mdns_ready || airplay.airplay_port == 0U ||
        airplay.ip_address == NULL || airplay.device_identifier == NULL ||
        airplay.public_key == NULL) {
        return false;
    }

    iap2_session_wifi_t wifi;
    if (!get_wifi(session, &wifi)) return false;

    const char *ip_addresses[] = {airplay.ip_address};
    iap2_carplay_start_session_config_t config = {
        .ssid = wifi.ssid,
        .passphrase = wifi.passphrase,
        .channel = wifi.channel,
        .security_type = wifi.security_type,
        .ip_addresses = ip_addresses,
        .ip_address_count = 1U,
        .airplay_port = airplay.airplay_port,
        .device_identifier = airplay.device_identifier,
        .public_key = airplay.public_key,
        .source_version = airplay.source_version,
    };
    uint8_t body[IAP2_SESSION_START_BODY_CAPACITY];
    size_t body_len = 0U;
    if (!iap2_carplay_build_start_session(&config, body, sizeof(body), &body_len)) {
        return false;
    }
    if (!send_csm(session, IAP2_MSG_CARPLAY_START_SESSION, body, body_len,
                  NULL, NULL, now_ms)) {
        return false;
    }
    session->start_session_sent = true;
    session->carplay_invite_active = true;
    session->carplay_invite_at_ms = now_ms;
    emit_event(session, reinvite ? IAP2_SESSION_EVENT_CARPLAY_REINVITE_SENT
                                 : IAP2_SESSION_EVENT_CARPLAY_INVITE_SENT);
    return true;
}

static void maybe_reinvite_carplay(iap2_session_t *session, uint64_t now_ms)
{
    if (!session->carplay_invite_active || !session->link_up ||
        session->airplay_event_connected ||
        !iap2_mfi_auth_is_authenticated(&session->auth) ||
        !session->identification_accepted) {
        return;
    }
    if (session->carplay_reinvite_count >= IAP2_SESSION_CARPLAY_REINVITE_MAX) return;
    if (now_ms - session->carplay_invite_at_ms <
        IAP2_SESSION_CARPLAY_REINVITE_INTERVAL_MS) {
        return;
    }
    ++session->carplay_reinvite_count;
    session->carplay_invite_at_ms = now_ms;
    session->start_session_sent = false;
    (void)send_carplay_start_session(session, now_ms, true);
}

static void handle_transport_notification(iap2_session_t *session,
                                          const uint8_t *body,
                                          size_t body_len,
                                          uint64_t now_ms)
{
    if (!iap2_param_validate(body, body_len)) return;
    session->transport_notification_seen = true;
    (void)send_wifi_configuration(session, true, now_ms);
}

static bool core_tx(const uint8_t *bytes, size_t len, void *context)
{
    iap2_session_t *session = context;
    return session->config.tx(bytes, len, session->config.context);
}

static void core_message(uint16_t message_id,
                         const uint8_t *body,
                         size_t body_len,
                         void *context)
{
    iap2_session_t *session = context;
    /* The link layer has no clock; periodic work keeps the invite timestamp
     * current, so use the invite clock when no advance ran yet. */
    iap2_session_handle_message(session, message_id, body, body_len,
                                session->now_ms);
}

static void core_state(iap2_core_state_t state, const char *reason, void *context)
{
    (void)reason;
    iap2_session_t *session = context;
    if (state == IAP2_CORE_READY) {
        emit_event(session, IAP2_SESSION_EVENT_LINK_READY);
    } else if (state == IAP2_CORE_DEAD) {
        session->link_up = false;
        reset_connection_state(session);
        emit_event(session, IAP2_SESSION_EVENT_LINK_DEAD);
    }
}

iap2_session_t *iap2_session_create(const iap2_session_config_t *config)
{
    if (config == NULL || config->tx == NULL) return NULL;
    iap2_session_t *session = calloc(1, sizeof(*session));
    if (session == NULL) return NULL;
    session->config = *config;
    session->core = iap2_core_create(core_tx, core_message, core_state, session);
    if (session->core == NULL) {
        free(session);
        return NULL;
    }
    return session;
}

void iap2_session_destroy(iap2_session_t *session)
{
    if (session == NULL) return;
    iap2_core_destroy(session->core);
    free(session);
}

void iap2_session_link_up(iap2_session_t *session, uint64_t now_ms)
{
    if (session == NULL) return;
    session->now_ms = now_ms;
    reset_connection_state(session);
    session->link_up = true;
    iap2_core_reset(session->core, "rfcomm_reconnect");
    (void)iap2_core_start(session->core, now_ms);
}

void iap2_session_link_down(iap2_session_t *session)
{
    if (session == NULL) return;
    session->link_up = false;
    reset_connection_state(session);
}

void iap2_session_feed(iap2_session_t *session, const uint8_t *bytes, size_t len,
                       uint64_t now_ms)
{
    if (session == NULL) return;
    session->now_ms = now_ms;
    iap2_core_feed(session->core, bytes, len, now_ms);
}

void iap2_session_advance(iap2_session_t *session, uint64_t now_ms)
{
    if (session == NULL) return;
    session->now_ms = now_ms;
    iap2_core_advance_time(session->core, now_ms);
    maybe_reinvite_carplay(session, now_ms);
}

void iap2_session_notify_airplay_event(iap2_session_t *session, bool connected,
                                       uint64_t now_ms)
{
    if (session == NULL) return;
    session->now_ms = now_ms;
    session->airplay_event_connected = connected;
    if (connected) {
        session->carplay_invite_active = false;
        session->carplay_reinvite_count = 0U;
        return;
    }
    if (session->link_up) {
        session->carplay_invite_active = true;
        session->carplay_invite_at_ms = now_ms;
    }
}

void iap2_session_handle_message(iap2_session_t *session,
                                 uint16_t message_id,
                                 const uint8_t *body,
                                 size_t body_len,
                                 uint64_t now_ms)
{
    if (session == NULL) return;
    session->now_ms = now_ms;
    switch (message_id) {
    case IAP2_MSG_START_IDENTIFICATION:
        (void)send_identification_information(session, now_ms);
        break;
    case IAP2_MSG_IDENTIFICATION_ACCEPTED:
        session->identification_accepted = true;
        emit_event(session, IAP2_SESSION_EVENT_IDENTIFICATION_ACCEPTED);
        maybe_send_wireless_subscriptions(session, now_ms);
        break;
    case IAP2_MSG_IDENTIFICATION_REJECTED:
        handle_identification_rejected(session, body, body_len, now_ms);
        break;
    case IAP2_MSG_MFI_AUTH_REQUEST:
        iap2_mfi_auth_reset(&session->auth);
        reset_wireless_negotiation(session);
        handle_authentication_certificate(session, now_ms);
        break;
    case IAP2_MSG_MFI_CHALLENGE:
        if (session->mfi_response_pending ||
            !iap2_mfi_auth_can_respond_to_challenge(&session->auth) ||
            !iap2_mfi_auth_mark_challenge_response_queued(&session->auth)) {
            emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_STATE_INVALID);
            break;
        }
        session->mfi_response_pending = true;
        if (!handle_authentication_challenge(session, body, body_len, now_ms)) {
            session->mfi_response_pending = false;
            iap2_mfi_auth_mark_failed(&session->auth);
            emit_event(session, IAP2_SESSION_EVENT_MFI_FAILED);
        }
        break;
    case IAP2_MSG_MFI_AUTH_FAILED:
        iap2_mfi_auth_mark_failed(&session->auth);
        reset_wireless_negotiation(session);
        emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_PHONE_REJECTED);
        break;
    case IAP2_MSG_MFI_AUTH_SUCCEEDED:
        if (!iap2_mfi_auth_mark_authenticated(&session->auth)) {
            emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_STATE_INVALID);
            break;
        }
        emit_event(session, IAP2_SESSION_EVENT_MFI_AUTHENTICATED);
        maybe_send_wireless_subscriptions(session, now_ms);
        break;
    case IAP2_MSG_WIFI_CONFIG_REQUEST:
        (void)send_wifi_configuration(session, session->transport_notification_seen,
                                      now_ms);
        break;
    case IAP2_MSG_CARPLAY_AVAILABILITY:
        if (!session->airplay_event_connected) {
            (void)send_carplay_start_session(session, now_ms, false);
        }
        break;
    case 0x4e0dU:
        /* Wireless CarPlay availability change: informational only. */
        break;
    case IAP2_MSG_TRANSPORT_NOTIFICATION:
        handle_transport_notification(session, body, body_len, now_ms);
        break;
    default:
        break;
    }
}

bool iap2_session_is_authenticated(const iap2_session_t *session)
{
    return session != NULL && iap2_mfi_auth_is_authenticated(&session->auth);
}

bool iap2_session_identification_accepted(const iap2_session_t *session)
{
    return session != NULL && session->identification_accepted;
}
