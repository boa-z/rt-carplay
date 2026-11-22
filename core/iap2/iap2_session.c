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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iap2_carplay.h"
#include "iap2_parameter.h"
#include "rtcp/log.h"
#include "rtcp/rtcp.h"

#define IAP2_SESSION_CERT_CAPACITY           1536U
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
#define IAP2_MSG_TRANSPORT_NOTIFICATION     0x4e0eU
#define IAP2_MSG_WIFI_CONFIG_REQUEST        0x5702U
#define IAP2_MSG_WIFI_CONFIGURATION         0x5703U
#define IAP2_MSG_START_NOW_PLAYING          0x5000U
#define IAP2_MSG_NOW_PLAYING_UPDATE         0x5001U
#define IAP2_MSG_ROUTE_GUIDANCE_UPDATE      0x5201U
#define IAP2_MSG_ROUTE_MANEUVER_UPDATE      0x5202U
#define IAP2_METADATA_TEXT_MAX              128U

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
    return session->identification_accepted && iap2_mfi_auth_is_authenticated(&session->auth);
}

static bool send_identification_information(iap2_session_t *session, uint64_t now_ms)
{
    iap2_identity_config_t identity = session->config.identity;
    identity.provide_location &= !session->location_component_rejected;
    identity.receive_device_time = session->config.on_device_time != NULL &&
                                   (session_is_wired(session) || session->config.airplay_tunnel);

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

static bool read_certificate(iap2_session_t *session,
                             uint8_t *certificate,
                             size_t *certificate_len)
{
    const struct rtcp_mfi *mfi = session->config.mfi;
    if (mfi == NULL || mfi->read_cert == NULL)
        return false;
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
    const struct rtcp_mfi *mfi = session->config.mfi;
    const uint8_t *challenge = NULL;
    size_t challenge_len = 0U;
    if (mfi == NULL || mfi->sign == NULL ||
        !iap2_param_find_single(body, body_len, 0, &challenge, &challenge_len) ||
        challenge_len == 0U || challenge_len > IAP2_SESSION_MAX_CHALLENGE_BYTES)
    {
        emit_event(session, IAP2_SESSION_EVENT_MFI_CHALLENGE_INVALID);
        return false;
    }

    uint8_t signature[IAP2_SESSION_SIGNATURE_CAPACITY];
    size_t signature_len = sizeof(signature);
    if (mfi->sign(challenge, challenge_len, signature, sizeof(signature),
                  &signature_len) != RTCP_OK ||
        signature_len == 0U || signature_len > sizeof(signature))
    {
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
        { 0, 4, 5, 0, 0, 0 },
        { 0, 1, 2, 3, 4, 11 },
    };
    static const uint8_t parameter_counts[] = { 3, 3, 6 };
    for (size_t i = 0; i < sizeof(message_ids) / sizeof(message_ids[0]); ++i)
    {
        if (message_is_omitted(session, message_ids[i]))
            continue;
        body_len = 0U;
        for (size_t j = 0; j < parameter_counts[i]; ++j)
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
        session->config.on_device_time(&update, session->config.context);
}

static void handle_now_playing(iap2_session_t *session, const uint8_t *body, size_t body_len)
{
    if (!session->config.on_now_playing || !metadata_ready(session))
        return;

    rtcp_now_playing_t update = { 0 };
    char title[IAP2_METADATA_TEXT_MAX + 1U];
    char artist[IAP2_METADATA_TEXT_MAX + 1U];
    char album[IAP2_METADATA_TEXT_MAX + 1U];
    const uint8_t *group, *status;
    size_t group_len, status_len;
    if (iap2_param_find_single(body, body_len, 0, &group, &group_len))
    {
        update.title = metadata_text(group, group_len, 1, title);
        update.artist = metadata_text(group, group_len, 12, artist);
        update.album = metadata_text(group, group_len, 6, album);
    }
    if (iap2_param_find_single(body, body_len, 1, &group, &group_len) &&
        iap2_param_find_single(group, group_len, 0, &status, &status_len) &&
        status_len == 1U && status[0] <= 4U)
    {
        update.has_playing = true;
        update.playing = status[0] == 1U;
    }
    bool usable = update.title || update.artist || update.album || update.has_playing;
    if (usable)
        session->config.on_now_playing(&update, session->config.context);
    if (session->now_playing_traces < 16U && (usable || !session->now_playing_empty_traced))
    {
        ++session->now_playing_traces;
        session->now_playing_empty_traced |= !usable;
        RTCP_LOGI("iap2", "now_playing rx=0x5001 bytes=%u title=%u artist=%u album=%u playback=%u",
                  (unsigned)body_len, update.title ? 1U : 0U, update.artist ? 1U : 0U,
                  update.album ? 1U : 0U, update.has_playing ? 1U : 0U);
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
    if (!session->config.on_route_guidance || !metadata_ready(session) ||
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
    if (update.has_state || update.road || update.destination || update.distance_remaining || update.distance_to_maneuver)
        session->config.on_route_guidance(&update, session->config.context);
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
        session->config.on_route_maneuver(&update, session->config.context);
}

static void handle_start_location(iap2_session_t *session, const uint8_t *body, size_t body_len)
{
    if (!session->config.identity.provide_location || session->location_component_rejected ||
        !metadata_ready(session) ||
        message_is_omitted(session, IAP2_MSG_START_LOCATION) ||
        message_is_omitted(session, IAP2_MSG_LOCATION) ||
        message_is_omitted(session, IAP2_MSG_STOP_LOCATION) || !iap2_param_validate(body, body_len))
        return;

    const uint8_t *data;
    size_t len;
    unsigned requested = 0;
    if (iap2_param_find_single(body, body_len, 1, &data, &len) && len == 0U)
        requested |= RTCP_LOCATION_GGA;
    if (iap2_param_find_single(body, body_len, 2, &data, &len) && len == 0U)
        requested |= RTCP_LOCATION_RMC;
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
    return session;
}

void iap2_session_destroy(iap2_session_t *session)
{
    if (session == NULL)
        return;
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
    reset_connection_state(session);
}

void iap2_session_feed(iap2_session_t *session, const uint8_t *bytes, size_t len,
                       uint64_t now_ms)
{
    if (session == NULL)
        return;
    session->now_ms = now_ms;
    iap2_core_feed(session->core, bytes, len, now_ms);
}

void iap2_session_advance(iap2_session_t *session, uint64_t now_ms)
{
    if (session == NULL)
        return;
    session->now_ms = now_ms;
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
    if (session == NULL)
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
    case IAP2_MSG_ROUTE_GUIDANCE_UPDATE:
        handle_route_guidance(session, body, body_len);
        break;
    case IAP2_MSG_ROUTE_MANEUVER_UPDATE:
        handle_route_maneuver(session, body, body_len);
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

    uint8_t body[4U + RTCP_LOCATION_NMEA_MAX + 1U];
    uint8_t text[RTCP_LOCATION_NMEA_MAX + 1U];
    memcpy(text, sentence, length);
    text[length] = '\0';
    size_t body_len = iap2_param_encode(body, sizeof(body), 0, text, length + 1U);
    return body_len != 0U && send_csm(session, IAP2_MSG_LOCATION, body, body_len, NULL, NULL, now_ms);
}
