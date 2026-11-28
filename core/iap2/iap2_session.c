/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Accessory-side iAP2 negotiation state machine.
 *
 * The message flow follows esp32-carplay-web-relay main/app_main.c
 * (GPL-3.0-or-later); platform services are injected through
 * iap2_session_config_t instead of ESP-IDF globals.  The same state machine
 * serves the Bluetooth RFCOMM (wireless) and USBMUX carkit (wired)
 * transports; only identification, the Wi-Fi exchange and the 0x4301 body
 * differ.
 */
#include "iap2_session.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iap2_carplay.h"
#include "iap2_parameter.h"
#include "rtcp/log.h"
#include "rtcp/rtcp.h"

#define IAP2_SESSION_CERT_CAPACITY           RTCP_MFI_WIRE_CERT_MAX
#define IAP2_SESSION_SIGNATURE_CAPACITY      512U
#define IAP2_SESSION_IDENTIFICATION_CAPACITY 1024U
#define IAP2_SESSION_WIFI_BODY_CAPACITY      256U
#define IAP2_SESSION_START_BODY_CAPACITY     1600U
#define IAP2_SESSION_MAX_CHALLENGE_BYTES     128U

#define IAP2_MSG_START_IDENTIFICATION       0x1d00U
#define IAP2_MSG_IDENTIFICATION_INFORMATION 0x1d01U
#define IAP2_MSG_IDENTIFICATION_ACCEPTED    0x1d02U
#define IAP2_MSG_IDENTIFICATION_REJECTED    0x1d03U
#define IAP2_MSG_MFI_AUTH_REQUEST           0xaa00U
#define IAP2_MSG_MFI_CERTIFICATE            0xaa01U
#define IAP2_MSG_MFI_CHALLENGE              0xaa02U
#define IAP2_MSG_MFI_CHALLENGE_RESPONSE     0xaa03U
#define IAP2_MSG_MFI_AUTH_FAILED            0xaa04U
#define IAP2_MSG_MFI_AUTH_SUCCEEDED         0xaa05U
#define IAP2_MSG_CARPLAY_AVAILABILITY       0x4300U
#define IAP2_MSG_CARPLAY_START_SESSION      0x4301U
#define IAP2_MSG_POWER_SOURCE_UPDATE        0xae03U
#define IAP2_MSG_POWER_UPDATE               0xae01U
#define IAP2_MSG_CELLULAR_UPDATE            0x4158U
#define IAP2_MSG_CALL_UPDATE                0x4155U
#define IAP2_MSG_TRANSPORT_NOTIFICATION     0x4e0eU
#define IAP2_MSG_WIFI_CONFIG_REQUEST        0x5702U
#define IAP2_MSG_WIFI_CONFIGURATION         0x5703U
#define IAP2_MSG_START_NOW_PLAYING          0x5000U
#define IAP2_MSG_NOW_PLAYING_UPDATE         0x5001U
#define IAP2_MSG_ROUTE_GUIDANCE_UPDATE      0x5201U
#define IAP2_MSG_ROUTE_MANEUVER_UPDATE      0x5202U
#define IAP2_METADATA_TEXT_MAX              128U
#define IAP2_MANEUVER_LIST_MAX              64U
#define IAP2_LANES_PER_GUIDANCE_MAX         16U
#define IAP2_ARTWORK_MAX_BYTES              (192U * 1024U)
#define IAP2_ARTWORK_TIMEOUT_MS             10000U

struct iap2_session
{
    iap2_session_config_t config;
    iap2_core_t *core;
    iap2_mfi_auth_t auth;

    uint16_t omitted_message_ids[IAP2_IDENTITY_MAX_OMITTED_MESSAGES];
    size_t omitted_message_count;
    bool location_component_rejected;
    unsigned identification_retries;

    bool link_up;
    bool identification_accepted;
    bool subscriptions_sent;
    bool wired_power_sent;
    bool start_session_sent;
    bool wired_reinvite_done;
    bool wired_transport_connected;
    bool wired_invite_pending;
    bool transport_notification_seen;
    bool mfi_response_pending;
    bool airplay_event_connected;
    bool carplay_invite_active;
    uint64_t now_ms;
    uint64_t carplay_invite_at_ms;
    unsigned carplay_reinvite_count;
    uint8_t pre_transport_wifi_configs;
    uint8_t post_transport_wifi_configs;
    uint8_t now_playing_traces;
    bool now_playing_empty_traced;
    unsigned location_requested;
    bool phone_status_reported;
    bool now_playing_reported;
    bool route_reported;
    bool device_time_reported;
    uint8_t *artwork;
    size_t artwork_size, artwork_received;
    uint64_t artwork_activity_ms;
    uint8_t artwork_id, track_artwork_id;
    bool artwork_complete, track_has_artwork_id;
};

static bool session_is_wired(const iap2_session_t *session)
{
    return session->config.transport == IAP2_SESSION_TRANSPORT_WIRED_USB;
}

static void emit_event(iap2_session_t *session, iap2_session_event_t event)
{
    if (session->config.on_event != NULL)
    {
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
    if (session->core == NULL)
        return false;
    bool queued = iap2_core_send_csm_with_ack_callback(session->core, message_id, body,
                                                       body_len, acked_callback,
                                                       acked_context, now_ms);
    if (queued && session->config.observe_message != NULL)
        session->config.observe_message(true, message_id, body, body_len,
                                        session->config.context);
    return queued;
}

static void clear_artwork(iap2_session_t *session)
{
    free(session->artwork);
    session->artwork = NULL;
    session->artwork_size = session->artwork_received = 0;
    session->artwork_complete = false;
}

static void invalidate_metadata(iap2_session_t *session)
{
    clear_artwork(session);
    session->track_has_artwork_id = false;
    if (session->device_time_reported)
    {
        session->device_time_reported = false;
        if (session->config.notify_device_time_invalidation)
            session->config.on_device_time(NULL, session->config.context);
    }
    if (session->phone_status_reported)
    {
        session->phone_status_reported = false;
        const rtcp_phone_status_t update = { .kind = RTCP_PHONE_STATUS_INVALIDATED };
        session->config.on_phone_status(&update, session->config.context);
    }
    if (session->now_playing_reported)
    {
        session->now_playing_reported = false;
        const rtcp_now_playing_details_t update = { .invalidated = true };
        session->config.on_now_playing_details(&update, session->config.context);
    }
    if (session->route_reported)
    {
        session->route_reported = false;
        const rtcp_route_details_t update = { .invalidated = true };
        session->config.on_route_details(&update, session->config.context);
    }
}

static void reset_wireless_negotiation(iap2_session_t *session)
{
    session->location_requested = 0;
    session->start_session_sent = false;
    session->wired_reinvite_done = false;
    session->wired_transport_connected = false;
    session->wired_invite_pending = false;
    session->subscriptions_sent = false;
    session->wired_power_sent = false;
    session->transport_notification_seen = false;
    session->pre_transport_wifi_configs = 0U;
    session->post_transport_wifi_configs = 0U;
    invalidate_metadata(session);
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
    session->location_component_rejected = false;
    session->identification_retries = 0U;
    session->identification_accepted = false;
}

static void reset_connection_state(iap2_session_t *session)
{
    session->now_playing_traces = 0;
    session->now_playing_empty_traced = false;
    iap2_mfi_auth_reset(&session->auth);
    session->mfi_response_pending = false;
    reset_identification(session);
    reset_wireless_negotiation(session);
}

static bool message_is_omitted(const iap2_session_t *session, uint16_t message_id)
{
    for (size_t i = 0; i < session->omitted_message_count; ++i)
    {
        if (session->omitted_message_ids[i] == message_id)
            return true;
    }
    return false;
}

static bool metadata_ready(const iap2_session_t *session)
{
    return session->link_up && session->identification_accepted &&
           iap2_mfi_auth_is_authenticated(&session->auth);
}

static bool send_identification_information(iap2_session_t *session, uint64_t now_ms)
{
    iap2_identity_config_t identity = session->config.identity;
    identity.provide_location &= !session->location_component_rejected;
    identity.provide_vehicle_speed &= session_is_wired(session) || session->config.airplay_tunnel;
    identity.receive_device_time = session->config.on_device_time != NULL &&
                                   (session_is_wired(session) || session->config.airplay_tunnel);
    identity.receive_lane_guidance = session->config.on_route_details && session->config.on_lane_guidance;

    uint8_t body[IAP2_SESSION_IDENTIFICATION_CAPACITY];
    size_t body_len = 0U;
    if (session_is_wired(session))
    {
        if (!iap2_identity_build_wired_omitting(
                &identity, session->config.car_play_interface_number,
                session->omitted_message_ids, session->omitted_message_count,
                body, sizeof(body), &body_len))
        {
            return false;
        }
    }
    else
    {
        if (identity.wifi_ssid == NULL || identity.wifi_ssid[0] == '\0')
            return false;
        if (!iap2_identity_build_wireless_omitting(&identity,
                                                   session->omitted_message_ids,
                                                   session->omitted_message_count,
                                                   body, sizeof(body), &body_len))
        {
            return false;
        }
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
    static const uint8_t location_component[] = { 0, 4, 0, 22 };
    if (session->config.identity.provide_location && !session->location_component_rejected &&
        body_len == sizeof(location_component) && body && memcmp(body, location_component, body_len) == 0 &&
        session->identification_retries < IAP2_SESSION_IDENTIFICATION_MAX_RETRIES)
    {
        session->location_component_rejected = true;
        ++session->identification_retries;
        (void)send_identification_information(session, now_ms);
        return;
    }
    if (!iap2_identity_parse_unsupported_messages(body, body_len, rejected,
                                                  IAP2_IDENTITY_MAX_OMITTED_MESSAGES,
                                                  &rejected_count))
    {
        emit_event(session, IAP2_SESSION_EVENT_IDENTIFICATION_REJECTED);
        return;
    }

    size_t added = 0U;
    for (size_t i = 0; i < rejected_count; ++i)
    {
        uint16_t id = rejected[i];
        if (message_is_omitted(session, id))
            continue;
        if (session->omitted_message_count >= IAP2_IDENTITY_MAX_OMITTED_MESSAGES)
        {
            emit_event(session, IAP2_SESSION_EVENT_IDENTIFICATION_REJECTED);
            return;
        }
        session->omitted_message_ids[session->omitted_message_count++] = id;
        ++added;
    }
    if (added == 0U ||
        session->identification_retries >= IAP2_SESSION_IDENTIFICATION_MAX_RETRIES)
    {
        emit_event(session, IAP2_SESSION_EVENT_IDENTIFICATION_REJECTED);
        return;
    }
    ++session->identification_retries;
    (void)send_identification_information(session, now_ms);
}

static int iap2_provider_info(const rtcp_mfi_provider_t *provider,
                              rtcp_mfi_provider_info_t *info)
{
    if (provider->get_info == NULL || provider->read_cert == NULL || provider->sign == NULL)
        return RTCP_EINVAL;
    int result = provider->get_info(provider->context, info);
    if (result != RTCP_OK)
        return result;
    if ((info->capabilities & RTCP_MFI_CAP_IAP2) == 0U ||
        (info->protocol_major != 2U && info->protocol_major != 3U))
        return RTCP_ENOTSUP;
    return RTCP_OK;
}

static bool read_certificate(iap2_session_t *session,
                             uint8_t *certificate,
                             size_t *certificate_len)
{
    const rtcp_mfi_provider_t *provider = session->config.mfi_provider;
    const struct rtcp_mfi *mfi = session->config.mfi;
    rtcp_mfi_provider_info_t info = { 0 };
    int result = RTCP_ENOTSUP;
    *certificate_len = 0U;
    if (provider != NULL)
    {
        result = iap2_provider_info(provider, &info);
        if (result == RTCP_OK)
            result = provider->read_cert(provider->context, certificate,
                                         IAP2_SESSION_CERT_CAPACITY, certificate_len);
    }
    else if (mfi != NULL && mfi->read_cert != NULL)
    {
        result = mfi->read_cert(certificate, IAP2_SESSION_CERT_CAPACITY, certificate_len);
    }
    if (result == RTCP_OK && (*certificate_len == 0U ||
                              *certificate_len > IAP2_SESSION_CERT_CAPACITY ||
                              (info.certificate_len != 0U && info.certificate_len != *certificate_len)))
        result = RTCP_EIO;
    if (result != RTCP_OK)
    {
        RTCP_LOGW("iap2", "auth=certificate_failed error=%d", result);
        return false;
    }
    return true;
}

static void handle_authentication_certificate(iap2_session_t *session,
                                              uint64_t now_ms)
{
    uint8_t certificate[IAP2_SESSION_CERT_CAPACITY];
    size_t certificate_len = 0U;
    if (!read_certificate(session, certificate, &certificate_len))
    {
        iap2_mfi_auth_mark_failed(&session->auth);
        emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_CERT_FAILED);
        return;
    }

    uint8_t body[IAP2_SESSION_CERT_CAPACITY + 4U];
    size_t body_len = iap2_param_encode(body, sizeof(body), 0,
                                        certificate, certificate_len);
    if (body_len == 0U)
    {
        iap2_mfi_auth_mark_failed(&session->auth);
        emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_CERT_FAILED);
        return;
    }
    if (send_csm(session, IAP2_MSG_MFI_CERTIFICATE, body, body_len,
                 NULL, NULL, now_ms))
    {
        emit_event(session, IAP2_SESSION_EVENT_MFI_CERTIFICATE_SENT);
    }
    else
    {
        iap2_mfi_auth_mark_failed(&session->auth);
        emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_CERT_FAILED);
    }
}

static void challenge_response_acked(uint16_t message_id, void *context)
{
    iap2_session_t *session = context;
    if (session == NULL)
        return;
    session->mfi_response_pending = false;
    if (message_id != IAP2_MSG_MFI_CHALLENGE_RESPONSE ||
        !iap2_mfi_auth_mark_challenge_response_acked(&session->auth))
    {
        iap2_mfi_auth_mark_failed(&session->auth);
        emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_ACK_FAILED);
    }
}

static bool handle_authentication_challenge(iap2_session_t *session,
                                            const uint8_t *body,
                                            size_t body_len,
                                            uint64_t now_ms)
{
    const rtcp_mfi_provider_t *provider = session->config.mfi_provider;
    const struct rtcp_mfi *mfi = session->config.mfi;
    const uint8_t *challenge = NULL;
    size_t challenge_len = 0U;
    if ((provider == NULL && (mfi == NULL || mfi->sign == NULL)) ||
        !iap2_param_find_single(body, body_len, 0, &challenge, &challenge_len) ||
        challenge_len == 0U || challenge_len > IAP2_SESSION_MAX_CHALLENGE_BYTES)
    {
        emit_event(session, IAP2_SESSION_EVENT_MFI_CHALLENGE_INVALID);
        return false;
    }

    uint8_t signature[IAP2_SESSION_SIGNATURE_CAPACITY];
    size_t signature_len = 0U;
    uint8_t protocol_major = 0U;
    int result;
    if (provider != NULL)
    {
        rtcp_mfi_provider_info_t info = { 0 };
        result = iap2_provider_info(provider, &info);
        protocol_major = info.protocol_major;
        if (result == RTCP_OK &&
            challenge_len != (protocol_major == 3U ? 32U : RTCP_MFI_CHALLENGE_LEN))
            result = RTCP_EINVAL;
        if (result == RTCP_OK)
            result = provider->sign(provider->context, challenge, challenge_len,
                                    signature, sizeof(signature), &signature_len);
    }
    else
    {
        result = mfi->sign(challenge, challenge_len, signature, sizeof(signature),
                           &signature_len);
    }
    if (result == RTCP_OK && (signature_len == 0U || signature_len > sizeof(signature) ||
                              (protocol_major == 3U && signature_len != 64U)))
        result = RTCP_EIO;
    if (result != RTCP_OK)
    {
        RTCP_LOGW("iap2", "auth=sign_failed error=%d", result);
        emit_event(session, IAP2_SESSION_EVENT_MFI_SIGN_FAILED);
        return false;
    }

    uint8_t response[IAP2_SESSION_SIGNATURE_CAPACITY + 4U];
    size_t response_len = iap2_param_encode(response, sizeof(response), 0,
                                            signature, signature_len);
    if (response_len != 0U &&
        send_csm(session, IAP2_MSG_MFI_CHALLENGE_RESPONSE, response,
                 response_len, challenge_response_acked, session, now_ms))
    {
        return true;
    }
    emit_event(session, IAP2_SESSION_EVENT_MFI_RESPONSE_SEND_FAILED);
    return false;
}

static void send_iap2_subscriptions(iap2_session_t *session, uint64_t now_ms)
{
    uint8_t body[64];
    size_t body_len = 0U;

    static const uint16_t now_playing_fields[] = { 1, 4, 6, 12, 26 };
    static const uint16_t playback_fields[] = { 0, 1, 7 };
    uint8_t group0[32];
    uint8_t group1[16];
    size_t group0_len = 0U;
    size_t group1_len = 0U;
    for (size_t i = 0; i < sizeof(now_playing_fields) / sizeof(now_playing_fields[0]); ++i)
    {
        (void)iap2_param_append_void(group0, sizeof(group0), &group0_len,
                                     now_playing_fields[i]);
    }
    for (size_t i = 0; i < sizeof(playback_fields) / sizeof(playback_fields[0]); ++i)
    {
        (void)iap2_param_append_void(group1, sizeof(group1), &group1_len,
                                     playback_fields[i]);
    }
    (void)iap2_param_append(body, sizeof(body), &body_len, 0, group0, group0_len);
    (void)iap2_param_append(body, sizeof(body), &body_len, 1, group1, group1_len);
    bool omitted = message_is_omitted(session, IAP2_MSG_START_NOW_PLAYING);
    bool queued = !omitted && send_csm(session, IAP2_MSG_START_NOW_PLAYING, body, body_len, NULL, NULL, now_ms);
    if (session->config.on_now_playing)
        RTCP_LOGI("iap2", "now_playing tx=0x5000 queued=%u omitted=%u", queued ? 1U : 0U, omitted ? 1U : 0U);

    body_len = 0U;
    static const uint8_t route_id[2] = { 0, IAP2_ROUTE_DISPLAY_ID };
    (void)iap2_param_append(body, sizeof(body), &body_len, 0, route_id, sizeof(route_id));
    (void)iap2_param_append_void(body, sizeof(body), &body_len, 1);
    (void)iap2_param_append_void(body, sizeof(body), &body_len, 2);
    if (!message_is_omitted(session, 0x5200U))
    {
        (void)send_csm(session, 0x5200U, body, body_len, NULL, NULL, now_ms);
    }

    static const uint16_t message_ids[] = { 0xae00U, 0x4157U, 0x4154U };
    static const uint8_t parameter_ids[][6] = {
        { 4, 5, 6, 0, 0, 0 },
        { 0, 4, 5, 1, 2, 0 },
        { 0, 1, 2, 3, 4, 11 },
    };
    static const uint8_t parameter_counts[] = { 3, 3, 6 };
    for (size_t i = 0; i < sizeof(message_ids) / sizeof(message_ids[0]); ++i)
    {
        if (message_is_omitted(session, message_ids[i]))
            continue;
        body_len = 0U;
        size_t count = parameter_counts[i];
        if (i == 1U && session->config.on_phone_status &&
            (session_is_wired(session) || session->config.airplay_tunnel))
            count = 5U;
        for (size_t j = 0; j < count; ++j)
        {
            (void)iap2_param_append_void(body, sizeof(body), &body_len,
                                         parameter_ids[i][j]);
        }
        (void)send_csm(session, message_ids[i], body, body_len, NULL, NULL, now_ms);
    }
}

/*
 * Wired sessions report their power source between authentication and the
 * subscriptions, as in the reference bring-up.  The values come from the
 * board configuration: zero means the accessory does not claim it can charge
 * the phone.
 */
static void send_wired_power_source(iap2_session_t *session, uint64_t now_ms)
{
    if (session->wired_power_sent)
        return;

    uint8_t body[16];
    size_t body_len = 0U;
    uint8_t current[2] = {
        (uint8_t)(session->config.available_current_ma >> 8),
        (uint8_t)session->config.available_current_ma,
    };
    uint8_t charge = session->config.battery_should_charge ? 1U : 0U;
    if (!iap2_param_append(body, sizeof(body), &body_len, 0, current,
                           sizeof(current)) ||
        !iap2_param_append(body, sizeof(body), &body_len, 1, &charge,
                           sizeof(charge)))
    {
        return;
    }
    if (send_csm(session, IAP2_MSG_POWER_SOURCE_UPDATE, body, body_len, NULL,
                 NULL, now_ms))
    {
        session->wired_power_sent = true;
    }
}

static void maybe_send_subscriptions(iap2_session_t *session, uint64_t now_ms)
{
    if (session->subscriptions_sent || !session->identification_accepted ||
        !iap2_mfi_auth_is_authenticated(&session->auth))
    {
        return;
    }
    if (session_is_wired(session))
    {
        send_wired_power_source(session, now_ms);
    }
    send_iap2_subscriptions(session, now_ms);
    session->subscriptions_sent = true;
    emit_event(session, IAP2_SESSION_EVENT_SUBSCRIPTIONS_SENT);
}

static bool metadata_utf8(const uint8_t *text, size_t length)
{
    for (size_t index = 0; index < length;)
    {
        unsigned lead = text[index++];
        if (lead < 0x80U)
            continue;
        if (lead < 0xc2U || lead > 0xf4U)
            return false;
        unsigned tail = lead < 0xe0U ? 1U : lead < 0xf0U ? 2U
                                                         : 3U;
        if (tail > length - index)
            return false;
        unsigned next = text[index];
        if ((lead == 0xe0U && next < 0xa0U) || (lead == 0xedU && next >= 0xa0U) ||
            (lead == 0xf0U && next < 0x90U) || (lead == 0xf4U && next >= 0x90U))
            return false;
        for (unsigned byte = 0; byte < tail; ++byte)
            if ((text[index++] & 0xc0U) != 0x80U)
                return false;
    }
    return true;
}

static const char *metadata_text(const uint8_t *body, size_t body_len, uint16_t id,
                                 char out[IAP2_METADATA_TEXT_MAX + 1U])
{
    const uint8_t *text;
    size_t length;
    if (!iap2_param_find_single(body, body_len, id, &text, &length))
        return NULL;
    if (length && text[length - 1] == 0)
        --length;
    if (length > IAP2_METADATA_TEXT_MAX || memchr(text, 0, length) || !metadata_utf8(text, length))
        return NULL;
    memcpy(out, text, length);
    out[length] = '\0';
    return out;
}

static void handle_device_time(iap2_session_t *session, const uint8_t *body, size_t body_len)
{
    if (!session->config.on_device_time || !metadata_ready(session) ||
        (!session_is_wired(session) && !session->config.airplay_tunnel) ||
        message_is_omitted(session, IAP2_MSG_DEVICE_TIME_UPDATE) ||
        !iap2_param_validate(body, body_len))
        return;

    rtcp_device_time_t update = { 0 };
    for (size_t offset = 0; offset < body_len;)
    {
        const uint8_t *parameter = body + offset;
        size_t length = ((size_t)parameter[0] << 8) | parameter[1];
        uint16_t id = ((uint16_t)parameter[2] << 8) | parameter[3];
        const uint8_t *value = parameter + 4;
        offset += length;
        switch (id)
        {
        case 0:
            if (length != 12U || update.has_unix_seconds || (value[0] & 0x80U))
                return;
            for (size_t index = 0; index < 8U; ++index)
                update.unix_seconds = (update.unix_seconds << 8) | value[index];
            update.has_unix_seconds = true;
            break;
        case 1:
            if (length != 6U || update.has_time_zone_offset)
                return;
            update.time_zone_offset_minutes =
                (int16_t)(((int)value[0] ^ 0x80) * 256 + value[1] - 32768);
            update.has_time_zone_offset = true;
            break;
        case 2:
            if (length != 5U || update.has_daylight_saving_offset)
                return;
            update.daylight_saving_offset_minutes = (int8_t)(((int)value[0] ^ 0x80) - 128);
            update.has_daylight_saving_offset = true;
            break;
        }
    }
    if (update.has_unix_seconds || update.has_time_zone_offset || update.has_daylight_saving_offset)
    {
        session->device_time_reported = true;
        session->config.on_device_time(&update, session->config.context);
    }
}

static bool metadata_byte(const uint8_t *body, size_t body_len, uint16_t id, uint8_t *out);
static bool metadata_uint(const uint8_t *body, size_t body_len, uint16_t id, size_t width, uint32_t *out);

static bool attach_artwork(iap2_session_t *session, rtcp_now_playing_details_t *details)
{
    if (!session->artwork_complete || !session->track_has_artwork_id ||
        session->track_artwork_id != session->artwork_id)
        return false;
    details->has_artwork = details->has_artwork_transfer_id = true;
    details->artwork_transfer_id = session->artwork_id;
    details->artwork = session->artwork;
    details->artwork_length = session->artwork_size;
    return true;
}

/* Classic File Transfer session 2. Setup declares a big-endian 64-bit size.
 * The phone waits for Accept, then marks the first and last data fragments. */
static void core_file(const uint8_t *payload, size_t length, void *context)
{
    iap2_session_t *session = context;
    if (length < 2U || !metadata_ready(session))
        return;
    if (session->artwork && session->now_ms - session->artwork_activity_ms >= IAP2_ARTWORK_TIMEOUT_MS)
        clear_artwork(session);
    uint8_t id = payload[0];
    uint8_t flags = payload[1];
    unsigned kind = flags & 0x0fU;
    if (kind == 4U)
    {
        if (length < 10U)
            return;
        uint64_t size = 0;
        for (size_t offset = 2; offset < 10; ++offset)
            size = (size << 8) | payload[offset];
        if (size == 0 || size > IAP2_ARTWORK_MAX_BYTES)
            return;
        clear_artwork(session);
        session->artwork = malloc((size_t)size);
        if (!session->artwork)
            return;
        session->artwork_id = id;
        session->artwork_size = (size_t)size;
        session->artwork_activity_ms = session->now_ms;
        if (!iap2_core_send_file_reply(session->core, id, 1, session->now_ms))
            clear_artwork(session);
        return;
    }
    if (!session->artwork || id != session->artwork_id)
        return;
    if (kind == 2U)
    {
        clear_artwork(session);
        return;
    }
    if (kind != 0U || session->artwork_complete)
        return;
    size_t fragment = length - 2U;
    bool first = (flags & 0x80U) != 0;
    bool last = (flags & 0x40U) != 0;
    if (!fragment || first != (session->artwork_received == 0) ||
        fragment > session->artwork_size - session->artwork_received ||
        last != (fragment == session->artwork_size - session->artwork_received))
    {
        clear_artwork(session);
        return;
    }
    memcpy(session->artwork + session->artwork_received, payload + 2U, fragment);
    session->artwork_received += fragment;
    session->artwork_activity_ms = session->now_ms;
    if (last)
    {
        if (!iap2_core_send_file_reply(session->core, id, 5, session->now_ms))
        {
            clear_artwork(session);
            return;
        }
        session->artwork_complete = true;
        RTCP_LOGI("iap2", "artwork complete id=%u bytes=%u", (unsigned)id,
                  (unsigned)session->artwork_size);
        rtcp_now_playing_details_t details = { 0 };
        if (attach_artwork(session, &details))
        {
            session->now_playing_reported = true;
            session->config.on_now_playing_details(&details, session->config.context);
            clear_artwork(session);
        }
    }
}

static void handle_now_playing(iap2_session_t *session, const uint8_t *body, size_t body_len)
{
    if ((!session->config.on_now_playing && !session->config.on_now_playing_details) ||
        !metadata_ready(session))
        return;

    rtcp_now_playing_details_t details = { 0 };
    rtcp_now_playing_t *update = &details.item;
    char title[IAP2_METADATA_TEXT_MAX + 1U];
    char artist[IAP2_METADATA_TEXT_MAX + 1U];
    char album[IAP2_METADATA_TEXT_MAX + 1U];
    const uint8_t *group, *status;
    size_t group_len, status_len;
    if (iap2_param_find_single(body, body_len, 0, &group, &group_len))
    {
        update->title = metadata_text(group, group_len, 1, title);
        update->artist = metadata_text(group, group_len, 12, artist);
        update->album = metadata_text(group, group_len, 6, album);
        details.has_duration = metadata_uint(group, group_len, 4, 4, &details.duration_ms);
        details.has_artwork_transfer_id = metadata_byte(group, group_len, 26, &details.artwork_transfer_id);
    }
    if (iap2_param_find_single(body, body_len, 1, &group, &group_len))
    {
        if (iap2_param_find_single(group, group_len, 0, &status, &status_len) &&
            status_len == 1U && status[0] <= 4U)
        {
            update->has_playing = true;
            update->playing = status[0] == 1U;
        }
        details.has_elapsed = metadata_uint(group, group_len, 1, 4, &details.elapsed_ms);
    }
    bool usable = update->title || update->artist || update->album || update->has_playing;
    if (usable && session->config.on_now_playing)
        session->config.on_now_playing(update, session->config.context);
    if (session->config.on_now_playing_details &&
        (usable || details.has_duration || details.has_elapsed || details.has_artwork_transfer_id))
    {
        if (details.has_artwork_transfer_id)
        {
            if (session->track_has_artwork_id && session->track_artwork_id != details.artwork_transfer_id)
                details.has_artwork = true; /* Clear the old cover while awaiting the new transfer. */
            session->track_artwork_id = details.artwork_transfer_id;
            session->track_has_artwork_id = true;
        }
        bool completed = attach_artwork(session, &details);
        session->now_playing_reported = true;
        session->config.on_now_playing_details(&details, session->config.context);
        if (completed)
            clear_artwork(session);
    }
    if (session->now_playing_traces < 16U && (usable || !session->now_playing_empty_traced))
    {
        ++session->now_playing_traces;
        session->now_playing_empty_traced |= !usable;
        RTCP_LOGI("iap2", "now_playing rx=0x5001 bytes=%u title=%u artist=%u album=%u playback=%u",
                  (unsigned)body_len, update->title ? 1U : 0U, update->artist ? 1U : 0U,
                  update->album ? 1U : 0U, update->has_playing ? 1U : 0U);
    }
}

static bool metadata_byte(const uint8_t *body, size_t body_len, uint16_t id, uint8_t *out)
{
    const uint8_t *data;
    size_t len;
    if (!iap2_param_find_single(body, body_len, id, &data, &len) || len != 1U)
        return false;
    *out = data[0];
    return true;
}

static bool metadata_uint(const uint8_t *body, size_t body_len, uint16_t id, size_t width, uint32_t *out)
{
    const uint8_t *data;
    size_t len;
    if (!iap2_param_find_single(body, body_len, id, &data, &len) || len != width)
        return false;
    uint32_t value = 0;
    for (size_t index = 0; index < width; ++index)
        value = (value << 8) | data[index];
    *out = value;
    return true;
}

static bool metadata_bool(const uint8_t *body, size_t body_len, uint16_t id, bool *out)
{
    uint8_t value;
    if (!metadata_byte(body, body_len, id, &value) || value > 1U)
        return false;
    *out = value != 0U;
    return true;
}

static void handle_phone_status(iap2_session_t *session, uint16_t message_id, const uint8_t *body, size_t body_len)
{
    uint16_t start_id = message_id == IAP2_MSG_POWER_UPDATE ? 0xae00U : (uint16_t)(message_id - 1U);
    if (!session->config.on_phone_status || !metadata_ready(session) ||
        (!session_is_wired(session) && !session->config.airplay_tunnel) ||
        message_is_omitted(session, message_id) || message_is_omitted(session, start_id) ||
        !iap2_param_validate(body, body_len))
        return;

    rtcp_phone_status_t update = { 0 };
    char first[IAP2_METADATA_TEXT_MAX + 1U], second[IAP2_METADATA_TEXT_MAX + 1U];
    char third[IAP2_METADATA_TEXT_MAX + 1U];
    switch (message_id)
    {
    case IAP2_MSG_POWER_UPDATE:
    {
        uint32_t level = 0;
        update.kind = RTCP_PHONE_STATUS_POWER;
        update.data.power.has_battery_level = metadata_uint(body, body_len, 6, 2, &level);
        update.data.power.battery_level = (uint16_t)level;
        update.data.power.has_charging_state = metadata_byte(body, body_len, 5, &update.data.power.charging_state);
        update.data.power.has_external_charger = metadata_bool(body, body_len, 4, &update.data.power.external_charger);
        if (!update.data.power.has_battery_level && !update.data.power.has_charging_state &&
            !update.data.power.has_external_charger)
            return;
        break;
    }
    case IAP2_MSG_CELLULAR_UPDATE:
        update.kind = RTCP_PHONE_STATUS_CELLULAR;
        (void)iap2_param_find_single(body, body_len, 0, &update.data.cellular.signal_strength,
                                     &update.data.cellular.signal_strength_count);
        (void)iap2_param_find_single(body, body_len, 1, &update.data.cellular.registration_status,
                                     &update.data.cellular.registration_status_count);
        update.data.cellular.carrier = metadata_text(body, body_len, 4, first);
        update.data.cellular.has_cellular_supported = metadata_bool(body, body_len, 5, &update.data.cellular.cellular_supported);
        update.data.cellular.has_airplane_mode = metadata_bool(body, body_len, 2, &update.data.cellular.airplane_mode);
        if (!update.data.cellular.signal_strength && !update.data.cellular.registration_status &&
            !update.data.cellular.carrier && !update.data.cellular.has_cellular_supported &&
            !update.data.cellular.has_airplane_mode)
            return;
        break;
    case IAP2_MSG_CALL_UPDATE:
        update.kind = RTCP_PHONE_STATUS_CALL;
        update.data.call.remote_id = metadata_text(body, body_len, 0, first);
        update.data.call.display_name = metadata_text(body, body_len, 1, second);
        update.data.call.uuid = metadata_text(body, body_len, 4, third);
        update.data.call.has_status = metadata_byte(body, body_len, 2, &update.data.call.status);
        update.data.call.has_direction = metadata_byte(body, body_len, 3, &update.data.call.direction);
        update.data.call.has_disconnect_reason = metadata_byte(body, body_len, 11, &update.data.call.disconnect_reason);
        if (!update.data.call.remote_id && !update.data.call.display_name && !update.data.call.uuid &&
            !update.data.call.has_status && !update.data.call.has_direction && !update.data.call.has_disconnect_reason)
            return;
        break;
    default:
        return;
    }
    session->phone_status_reported = true;
    session->config.on_phone_status(&update, session->config.context);
}

static bool route_display_matches(const uint8_t *body, size_t body_len)
{
    size_t offset = 0;
    bool found = false;
    while (offset < body_len)
    {
        const uint8_t *parameter = body + offset;
        if (body_len - offset < 4U)
            return false;
        size_t length = ((size_t)parameter[0] << 8) | parameter[1];
        if (length < 4U || length > body_len - offset)
            return false;
        if (parameter[2] == 0 && parameter[3] == 0)
        {
            if (found || length != 6U || parameter[4] != 0 || parameter[5] != IAP2_ROUTE_DISPLAY_ID)
                return false;
            found = true;
        }
        offset += length;
    }
    return true;
}

static void handle_route_guidance(iap2_session_t *session, const uint8_t *body, size_t body_len)
{
    if ((!session->config.on_route_guidance && !session->config.on_route_details) || !metadata_ready(session) ||
        !body || !route_display_matches(body, body_len))
        return;

    char road[IAP2_METADATA_TEXT_MAX + 1U], destination[IAP2_METADATA_TEXT_MAX + 1U];
    char remaining[IAP2_METADATA_TEXT_MAX + 1U], maneuver[IAP2_METADATA_TEXT_MAX + 1U];
    rtcp_route_guidance_t update = {
        .road = metadata_text(body, body_len, 3, road),
        .destination = metadata_text(body, body_len, 4, destination),
        .distance_remaining = metadata_text(body, body_len, 8, remaining),
        .distance_to_maneuver = metadata_text(body, body_len, 11, maneuver),
    };
    update.has_state = metadata_byte(body, body_len, 1, &update.state);
    bool usable = update.has_state || update.road || update.destination ||
                  update.distance_remaining || update.distance_to_maneuver;
    if (usable && session->config.on_route_guidance)
        session->config.on_route_guidance(&update, session->config.context);
    if (!session->config.on_route_details)
        return;

    rtcp_route_details_t details = { .guidance = update, .invalidated = update.has_state && update.state == 0U };
    details.has_distance_remaining = metadata_uint(body, body_len, 7, 4, &details.distance_remaining);
    details.has_distance_to_maneuver = metadata_uint(body, body_len, 10, 4, &details.distance_to_maneuver);
    details.has_remaining_units = metadata_byte(body, body_len, 9, &details.remaining_units);
    details.has_maneuver_units = metadata_byte(body, body_len, 12, &details.maneuver_units);
    uint32_t count = 0;
    details.has_maneuver_count = metadata_uint(body, body_len, 14, 2, &count);
    details.maneuver_count = (uint16_t)count;
    details.has_lane_index = metadata_uint(body, body_len, 16, 2, &count);
    details.lane_index = details.has_lane_index ? (uint16_t)count : 0;
    details.has_lane_count = metadata_uint(body, body_len, 17, 2, &count);
    details.lane_count = details.has_lane_count ? (uint16_t)count : 0;
    details.has_lanes_shown = metadata_bool(body, body_len, 18, &details.lanes_shown);
    uint16_t indices[IAP2_MANEUVER_LIST_MAX];
    const uint8_t *list;
    size_t list_len;
    /* A zero-length TLV clears the order. A missing or malformed one leaves it unchanged. */
    if (iap2_param_find_single(body, body_len, 13, &list, &list_len) &&
        list_len % 2U == 0U && list_len / 2U <= IAP2_MANEUVER_LIST_MAX)
    {
        details.maneuver_indices = indices;
        details.maneuver_index_count = list_len / 2U;
        for (size_t index = 0; index < details.maneuver_index_count; ++index)
            indices[index] = ((uint16_t)list[index * 2U] << 8) | list[index * 2U + 1U];
    }
    if (usable || details.has_distance_remaining || details.has_distance_to_maneuver ||
        details.has_remaining_units || details.has_maneuver_units || details.has_maneuver_count ||
        details.maneuver_indices || details.has_lane_index || details.has_lane_count || details.has_lanes_shown)
    {
        session->route_reported = !details.invalidated;
        session->config.on_route_details(&details, session->config.context);
    }
}

static void handle_route_maneuver(iap2_session_t *session, const uint8_t *body, size_t body_len)
{
    const uint8_t *index;
    size_t index_len;
    if (!session->config.on_route_maneuver || !metadata_ready(session) ||
        !iap2_param_find_single(body, body_len, 1, &index, &index_len) || index_len != 2U ||
        !route_display_matches(body, body_len))
        return;

    char description[IAP2_METADATA_TEXT_MAX + 1U], road[IAP2_METADATA_TEXT_MAX + 1U];
    char distance[IAP2_METADATA_TEXT_MAX + 1U];
    rtcp_route_maneuver_t update = {
        .index = (uint16_t)(((uint16_t)index[0] << 8) | index[1]),
        .description = metadata_text(body, body_len, 2, description),
        .road_after = metadata_text(body, body_len, 4, road),
        .distance = metadata_text(body, body_len, 6, distance),
    };
    update.has_type = metadata_byte(body, body_len, 3, &update.type);
    if (update.has_type || update.description || update.road_after || update.distance)
    {
        if (session->config.on_route_details)
            session->route_reported = true;
        session->config.on_route_maneuver(&update, session->config.context);
    }
}

static int16_t signed_be16(const uint8_t *value)
{
    return (int16_t)(((int)value[0] ^ 0x80) * 256 + value[1] - 32768);
}

static bool parse_lane(const uint8_t *body, size_t body_len, rtcp_lane_t *lane)
{
    uint32_t index;
    if (!metadata_uint(body, body_len, 0, 2, &index))
        return false;
    lane->index = (uint16_t)index;
    lane->has_status = metadata_byte(body, body_len, 1, &lane->status);
    const uint8_t *value;
    size_t value_len;
    if (iap2_param_find_single(body, body_len, 3, &value, &value_len) && value_len == 2U)
    {
        lane->has_highlight_angle = true;
        lane->highlight_angle = signed_be16(value);
    }
    for (size_t offset = 0; offset < body_len;)
    {
        const uint8_t *parameter = body + offset;
        size_t length = ((size_t)parameter[0] << 8) | parameter[1];
        if (parameter[2] == 0U && parameter[3] == 2U)
        {
            if (length != 6U || lane->angle_count == RTCP_LANE_MAX_ANGLES)
                return false;
            lane->angles[lane->angle_count++] = signed_be16(parameter + 4);
        }
        offset += length;
    }
    return true;
}

static void handle_lane_guidance(iap2_session_t *session, const uint8_t *body, size_t body_len)
{
    uint32_t index;
    if (!session->config.on_lane_guidance || !session->config.on_route_details ||
        !metadata_ready(session) || message_is_omitted(session, IAP2_MSG_LANE_GUIDANCE) ||
        !metadata_uint(body, body_len, 1, 2, &index) || !route_display_matches(body, body_len))
        return;

    rtcp_lane_t lanes[IAP2_LANES_PER_GUIDANCE_MAX] = { 0 };
    char description[IAP2_METADATA_TEXT_MAX + 1U];
    rtcp_lane_guidance_t update = { .index = (uint16_t)index, .description = metadata_text(body, body_len, 3, description) };
    for (size_t offset = 0; offset < body_len;)
    {
        const uint8_t *parameter = body + offset;
        size_t length = ((size_t)parameter[0] << 8) | parameter[1];
        if (parameter[2] == 0U && parameter[3] == 2U)
        {
            if (update.lane_count == IAP2_LANES_PER_GUIDANCE_MAX ||
                !parse_lane(parameter + 4, length - 4U, &lanes[update.lane_count]))
                return;
            ++update.lane_count;
            update.lanes = lanes;
        }
        offset += length;
    }
    if (update.description || update.lanes)
    {
        session->route_reported = true;
        session->config.on_lane_guidance(&update, session->config.context);
    }
}

static void handle_start_location(iap2_session_t *session, const uint8_t *body, size_t body_len)
{
    if (!session->config.identity.provide_location || session->location_component_rejected ||
        !metadata_ready(session) ||
        message_is_omitted(session, IAP2_MSG_START_LOCATION) ||
        message_is_omitted(session, IAP2_MSG_LOCATION) ||
        message_is_omitted(session, IAP2_MSG_STOP_LOCATION) || !iap2_param_validate(body, body_len))
        return;

    /* Validate the complete selector list before replacing the active
     * subscription. Malformed or duplicate selectors leave it intact. */
    unsigned requested = 0;
    for (size_t offset = 0; offset < body_len;)
    {
        const uint8_t *parameter = body + offset;
        size_t length = ((size_t)parameter[0] << 8) | parameter[1];
        unsigned id = ((unsigned)parameter[2] << 8) | parameter[3];
        unsigned kind = 0;
        switch (id)
        {
        case 1:
            kind = RTCP_LOCATION_GGA;
            break;
        case 2:
            kind = RTCP_LOCATION_RMC;
            break;
        case 4:
            kind = RTCP_LOCATION_PASCD;
            break;
        }
        if (kind && (length != 4U || (requested & kind)))
            return;
        requested |= kind;
        offset += length;
    }
    if (!session->config.identity.provide_vehicle_speed ||
        (!session_is_wired(session) && !session->config.airplay_tunnel))
        requested &= ~RTCP_LOCATION_PASCD;
    session->location_requested = requested;
}

static bool get_wifi(iap2_session_t *session, iap2_session_wifi_t *wifi)
{
    memset(wifi, 0, sizeof(*wifi));
    if (session->config.get_wifi == NULL ||
        !session->config.get_wifi(wifi, session->config.context))
    {
        return false;
    }
    return wifi->ssid != NULL && wifi->ssid[0] != '\0' &&
           wifi->passphrase != NULL;
}

static bool send_wifi_configuration(iap2_session_t *session,
                                    bool post_transport,
                                    uint64_t now_ms)
{
    /* Wired CarPlay has no accessory SoftAP: the network runs over NCM. */
    if (session_is_wired(session))
        return false;
    if (!iap2_mfi_auth_is_authenticated(&session->auth))
        return false;

    uint8_t *count = post_transport ? &session->post_transport_wifi_configs
                                    : &session->pre_transport_wifi_configs;
    uint8_t limit = post_transport ? IAP2_SESSION_POST_TRANSPORT_WIFI_CONFIG_LIMIT
                                   : IAP2_SESSION_PRE_TRANSPORT_WIFI_CONFIG_LIMIT;
    if (*count >= limit)
        return false;

    iap2_session_wifi_t wifi;
    if (!get_wifi(session, &wifi))
        return false;

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
                                               &body_len))
    {
        return false;
    }
    if (!send_csm(session, IAP2_MSG_WIFI_CONFIGURATION, body, body_len,
                  NULL, NULL, now_ms))
    {
        return false;
    }
    ++*count;
    emit_event(session, IAP2_SESSION_EVENT_WIFI_CONFIG_SENT);
    return true;
}

static void carplay_invite_acked(uint16_t message_id, void *context)
{
    if (message_id == IAP2_MSG_CARPLAY_START_SESSION)
        emit_event(context, IAP2_SESSION_EVENT_CARPLAY_INVITE_ACKED);
}

static bool send_carplay_start_session(iap2_session_t *session, uint64_t now_ms,
                                       bool reinvite)
{
    if (!iap2_mfi_auth_is_authenticated(&session->auth))
        return false;
    if (session->start_session_sent)
        return false;

    const bool wired = session_is_wired(session);

    iap2_session_airplay_t airplay;
    memset(&airplay, 0, sizeof(airplay));
    if (session->config.get_airplay == NULL ||
        !session->config.get_airplay(&airplay, session->config.context) ||
        (!wired && !airplay.mdns_ready) || airplay.airplay_port == 0U ||
        airplay.ip_address == NULL || airplay.device_identifier == NULL ||
        airplay.public_key == NULL)
    {
        return false;
    }

    if (wired)
        session->wired_transport_connected = airplay.transport_connected || airplay.event_connected;
    if (wired && session->wired_transport_connected)
    {
        session->carplay_invite_active = false;
        session->wired_invite_pending = false;
        return false;
    }

    iap2_session_wifi_t wifi;
    memset(&wifi, 0, sizeof(wifi));
    if (!wired && !get_wifi(session, &wifi))
        return false;

    const char *ip_addresses[] = { airplay.ip_address };
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
    bool built = wired
                     ? iap2_carplay_build_start_session_wired(&config, body, sizeof(body), &body_len)
                     : iap2_carplay_build_start_session(&config, body, sizeof(body), &body_len);
    if (!built)
    {
        return false;
    }
    if (!send_csm(session, IAP2_MSG_CARPLAY_START_SESSION, body, body_len,
                  carplay_invite_acked, session, now_ms))
    {
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
    if (session_is_wired(session))
    {
        uint32_t delay = session->config.wired_reinvite_delay_ms;
        if (!delay || session->wired_reinvite_done ||
            !session->carplay_invite_active || !session->link_up ||
            session->airplay_event_connected ||
            now_ms - session->carplay_invite_at_ms < delay)
            return;
        /* One application-enabled compatibility retry, never a periodic
         * wired invite loop. get_airplay also checks for accepted TCP. */
        session->start_session_sent = false;
        if (send_carplay_start_session(session, now_ms, true) ||
            !session->carplay_invite_active)
            session->wired_reinvite_done = true;
        return;
    }
    if (!session->carplay_invite_active || !session->link_up ||
        session->airplay_event_connected ||
        !iap2_mfi_auth_is_authenticated(&session->auth) ||
        !session->identification_accepted)
    {
        return;
    }
    if (session->carplay_reinvite_count >= IAP2_SESSION_CARPLAY_REINVITE_MAX)
        return;
    if (now_ms - session->carplay_invite_at_ms <
        IAP2_SESSION_CARPLAY_REINVITE_INTERVAL_MS)
    {
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
    if (!iap2_param_validate(body, body_len))
        return;
    session->transport_notification_seen = true;
    if (!session_is_wired(session))
    {
        (void)send_wifi_configuration(session, true, now_ms);
    }
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
    if (state == IAP2_CORE_READY)
    {
        emit_event(session, IAP2_SESSION_EVENT_LINK_READY);
    }
    else if (state == IAP2_CORE_DEAD)
    {
        session->link_up = false;
        reset_connection_state(session);
        emit_event(session, IAP2_SESSION_EVENT_LINK_DEAD);
    }
}

iap2_session_t *iap2_session_create(const iap2_session_config_t *config)
{
    if (config == NULL || config->tx == NULL)
        return NULL;
    iap2_session_t *session = calloc(1, sizeof(*session));
    if (session == NULL)
        return NULL;
    session->config = *config;
    session->core = iap2_core_create(core_tx, core_message, core_state, session);
    if (session->core == NULL)
    {
        free(session);
        return NULL;
    }
    if (config->airplay_tunnel)
        iap2_core_set_tcp_transport(session->core);
    if (config->on_now_playing_details && (config->airplay_tunnel || session_is_wired(session)))
        iap2_core_set_file_handler(session->core, core_file);
    return session;
}

void iap2_session_destroy(iap2_session_t *session)
{
    if (session == NULL)
        return;
    clear_artwork(session);
    iap2_core_destroy(session->core);
    free(session);
}

void iap2_session_link_up(iap2_session_t *session, uint64_t now_ms)
{
    if (session == NULL)
        return;
    session->now_ms = now_ms;
    reset_connection_state(session);
    session->link_up = true;
    iap2_core_reset(session->core, "rfcomm_reconnect");
    (void)iap2_core_start(session->core, now_ms);
}

void iap2_session_link_down(iap2_session_t *session)
{
    if (session == NULL)
        return;
    session->link_up = false;
    iap2_core_reset(session->core, "transport_down");
    if (session_is_wired(session) || session->config.airplay_tunnel)
    {
        session->airplay_event_connected = false;
        session->carplay_invite_active = false;
        session->carplay_invite_at_ms = 0;
        session->carplay_reinvite_count = 0;
    }
    reset_connection_state(session);
}

void iap2_session_feed(iap2_session_t *session, const uint8_t *bytes, size_t len,
                       uint64_t now_ms)
{
    if (session == NULL || !session->link_up)
        return;
    session->now_ms = now_ms;
    iap2_core_feed(session->core, bytes, len, now_ms);
}

void iap2_session_advance(iap2_session_t *session, uint64_t now_ms)
{
    if (session == NULL || !session->link_up)
        return;
    session->now_ms = now_ms;
    if (session->artwork && now_ms - session->artwork_activity_ms >= IAP2_ARTWORK_TIMEOUT_MS)
        clear_artwork(session);
    iap2_core_advance_time(session->core, now_ms);
    /* An availability message can precede the host's valid IPv6 address or
     * MFi completion. Retain this one invitation until prerequisites are
     * ready; never turn it into periodic wired re-invites. */
    if (session_is_wired(session) && session->link_up &&
        session->wired_invite_pending && !session->airplay_event_connected &&
        send_carplay_start_session(session, now_ms, false))
    {
        session->wired_invite_pending = false;
    }
    maybe_reinvite_carplay(session, now_ms);
}

void iap2_session_notify_airplay_event(iap2_session_t *session, bool connected,
                                       uint64_t now_ms)
{
    if (session == NULL)
        return;
    session->now_ms = now_ms;
    session->airplay_event_connected = connected;
    if (session_is_wired(session))
    {
        /* Wired availability is driven by 0x4300, not by invite pacing. */
        return;
    }
    if (connected)
    {
        session->carplay_invite_active = false;
        session->carplay_reinvite_count = 0U;
        return;
    }
    if (session->link_up)
    {
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
    if (session == NULL || !session->link_up)
        return;
    session->now_ms = now_ms;
    if (session->config.observe_message != NULL)
        session->config.observe_message(false, message_id, body, body_len,
                                        session->config.context);
    switch (message_id)
    {
    case IAP2_MSG_START_IDENTIFICATION:
        (void)send_identification_information(session, now_ms);
        break;
    case IAP2_MSG_IDENTIFICATION_ACCEPTED:
        session->identification_accepted = true;
        emit_event(session, IAP2_SESSION_EVENT_IDENTIFICATION_ACCEPTED);
        maybe_send_subscriptions(session, now_ms);
        break;
    case IAP2_MSG_IDENTIFICATION_REJECTED:
        session->identification_accepted = false;
        session->location_requested = 0;
        invalidate_metadata(session);
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
            !iap2_mfi_auth_mark_challenge_response_queued(&session->auth))
        {
            emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_STATE_INVALID);
            break;
        }
        session->mfi_response_pending = true;
        if (!handle_authentication_challenge(session, body, body_len, now_ms))
        {
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
        if (!iap2_mfi_auth_mark_authenticated(&session->auth))
        {
            emit_mfi_failure(session, IAP2_SESSION_EVENT_MFI_STATE_INVALID);
            break;
        }
        emit_event(session, IAP2_SESSION_EVENT_MFI_AUTHENTICATED);
        maybe_send_subscriptions(session, now_ms);
        break;
    case IAP2_MSG_WIFI_CONFIG_REQUEST:
        (void)send_wifi_configuration(session, session->transport_notification_seen,
                                      now_ms);
        break;
    case IAP2_MSG_CARPLAY_AVAILABILITY:
        emit_event(session, IAP2_SESSION_EVENT_CARPLAY_AVAILABILITY_RECEIVED);
        if (!session->airplay_event_connected)
        {
            /* Each new wired availability deserves a fresh answer. The
             * sent flag suppresses timer duplicates, not peer requests. */
            if (session_is_wired(session))
                session->start_session_sent = false;
            bool sent = send_carplay_start_session(session, now_ms, false);
            if (session_is_wired(session))
                session->wired_invite_pending = !sent && !session->start_session_sent &&
                                                !session->wired_transport_connected;
        }
        break;
    case 0x4e0dU:
        /* Wireless CarPlay availability change: informational only. */
        break;
    case IAP2_MSG_TRANSPORT_NOTIFICATION:
        handle_transport_notification(session, body, body_len, now_ms);
        break;
    case IAP2_MSG_NOW_PLAYING_UPDATE:
        handle_now_playing(session, body, body_len);
        break;
    case IAP2_MSG_DEVICE_TIME_UPDATE:
        handle_device_time(session, body, body_len);
        break;
    case IAP2_MSG_POWER_UPDATE:
    case IAP2_MSG_CELLULAR_UPDATE:
    case IAP2_MSG_CALL_UPDATE:
        handle_phone_status(session, message_id, body, body_len);
        break;
    case IAP2_MSG_ROUTE_GUIDANCE_UPDATE:
        handle_route_guidance(session, body, body_len);
        break;
    case IAP2_MSG_ROUTE_MANEUVER_UPDATE:
        handle_route_maneuver(session, body, body_len);
        break;
    case IAP2_MSG_LANE_GUIDANCE:
        handle_lane_guidance(session, body, body_len);
        break;
    case IAP2_MSG_START_LOCATION:
        handle_start_location(session, body, body_len);
        break;
    case IAP2_MSG_STOP_LOCATION:
        if (body_len == 0U)
            session->location_requested = 0;
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

unsigned iap2_session_location_requested(const iap2_session_t *session)
{
    return session && session->link_up && metadata_ready(session) ? session->location_requested : 0U;
}

static int nmea_hex(char digit)
{
    if (digit >= '0' && digit <= '9')
        return digit - '0';
    if (digit >= 'A' && digit <= 'F')
        return digit - 'A' + 10;
    return -1;
}

static bool send_location_text(iap2_session_t *session, const char *sentence, size_t length, uint64_t now_ms)
{
    uint8_t body[4U + RTCP_LOCATION_NMEA_MAX + 1U];
    size_t body_len = iap2_param_encode(body, sizeof(body), 0, (const uint8_t *)sentence, length + 1U);
    return body_len != 0U && send_csm(session, IAP2_MSG_LOCATION, body, body_len, NULL, NULL, now_ms);
}

bool iap2_session_send_location(iap2_session_t *session, const char *sentence, size_t length, uint64_t now_ms)
{
    unsigned requested = iap2_session_location_requested(session);
    if (!requested || !sentence || length < 10U || length > RTCP_LOCATION_NMEA_MAX ||
        sentence[0] != '$' || sentence[6] != ',' || sentence[length - 3U] != '*')
        return false;

    unsigned kind = 0;
    if (memcmp(sentence + 3, "GGA", 3) == 0)
        kind = RTCP_LOCATION_GGA;
    else if (memcmp(sentence + 3, "RMC", 3) == 0)
        kind = RTCP_LOCATION_RMC;
    if (!(requested & kind))
        return false;

    unsigned checksum = 0;
    for (size_t offset = 1; offset < length - 3U; ++offset)
    {
        unsigned char byte = (unsigned char)sentence[offset];
        if (byte < 0x20U || byte > 0x7eU || byte == '*' || byte == '$')
            return false;
        checksum ^= byte;
    }
    int high = nmea_hex(sentence[length - 2U]);
    int low = nmea_hex(sentence[length - 1U]);
    if (high < 0 || low < 0 || checksum != (unsigned)(16 * high + low))
        return false;

    char text[RTCP_LOCATION_NMEA_MAX + 1U];
    memcpy(text, sentence, length);
    text[length] = '\0';
    return send_location_text(session, text, length, now_ms);
}

bool iap2_session_send_vehicle_speed(iap2_session_t *session, uint64_t sample_ms,
                                     rtcp_vehicle_gear_t gear, uint32_t speed_mm_s, uint64_t now_ms)
{
    if (!(iap2_session_location_requested(session) & RTCP_LOCATION_PASCD) ||
        (gear != RTCP_GEAR_PARK && gear != RTCP_GEAR_REVERSE &&
         gear != RTCP_GEAR_NEUTRAL && gear != RTCP_GEAR_DRIVE))
        return false;

    char sentence[RTCP_LOCATION_NMEA_MAX + 1U];
    /* DiPlay's captured single-sample layout uses C and the fixed 0,1,0.00 fields. */
    int length = snprintf(sentence, sizeof(sentence), "$PASCD,%" PRIu64 ".%03u,C,%c,0,1,0.00,%" PRIu32 ".%03u",
                          sample_ms / 1000U, (unsigned)(sample_ms % 1000U), (int)gear,
                          speed_mm_s / 1000U, (unsigned)(speed_mm_s % 1000U));
    if (length < 0 || (size_t)length + 5U >= sizeof(sentence))
        return false;
    unsigned checksum = 0;
    for (int index = 1; index < length; ++index)
        checksum ^= (unsigned char)sentence[index];
    if (snprintf(sentence + length, sizeof(sentence) - (size_t)length, "*%02X\r\n", checksum) != 5)
        return false;
    return send_location_text(session, sentence, (size_t)length + 5U, now_ms);
}
