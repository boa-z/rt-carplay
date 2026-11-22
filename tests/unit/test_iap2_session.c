/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iap2_session.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

#define TX_CAPACITY           65536U
#define MAX_RECORDED_MESSAGES 64U

typedef struct
{
    uint16_t id;
    uint8_t body[1600];
    size_t body_len;
} csm_record_t;

typedef struct
{
    uint8_t tx[TX_CAPACITY];
    size_t tx_len;
    size_t scanned;
    uint8_t peer_seq;
    csm_record_t messages[MAX_RECORDED_MESSAGES];
    size_t message_count;
    unsigned events[64];
    unsigned event_count;
    unsigned observed_rx, observed_tx;
    csm_record_t observed;
    unsigned now_playing_updates;
    char title[129], artist[129], album[129];
    bool playing;
    unsigned routes, maneuvers;
    char destination[129], remaining[129], instruction[129];
    uint8_t route_state;
    uint16_t maneuver_index;
    unsigned device_time_updates;
    unsigned device_time_invalidations;
    rtcp_device_time_t device_time;
    unsigned status_updates[4];
    unsigned media_details, media_invalidations, route_details, route_invalidations;
    unsigned lane_guidance;
} harness_t;

static harness_t g_harness;

static const uint8_t device_time_fixture[] = {
    0,
    12,
    0,
    0,
    0,
    0,
    0,
    0,
    0x68,
    0x82,
    0xc2,
    0xc0,
    0,
    6,
    0,
    1,
    0xfe,
    0x20,
    0,
    5,
    0,
    2,
    60,
};

static void capture_device_time(const rtcp_device_time_t *update, void *context)
{
    harness_t *harness = context;
    if (!update)
    {
        ++harness->device_time_invalidations;
        return;
    }
    ++harness->device_time_updates;
    harness->device_time = *update;
}

static void capture_phone_status(const rtcp_phone_status_t *update, void *context)
{
    harness_t *harness = context;
    ++harness->status_updates[update->kind];
    switch (update->kind)
    {
    case RTCP_PHONE_STATUS_POWER:
        assert(update->data.power.has_battery_level && update->data.power.battery_level == 76U);
        assert(update->data.power.has_external_charger && !update->data.power.external_charger);
        break;
    case RTCP_PHONE_STATUS_CELLULAR:
        assert(update->data.cellular.signal_strength_count == 2U && update->data.cellular.signal_strength[1] == 3U);
        assert(update->data.cellular.has_airplane_mode && update->data.cellular.airplane_mode);
        break;
    case RTCP_PHONE_STATUS_CALL:
        assert(update->data.call.has_status && update->data.call.status == 0U);
        assert(!strcmp(update->data.call.uuid, "call"));
        break;
    case RTCP_PHONE_STATUS_INVALIDATED:
        break;
    }
}

static void capture_media_details(const rtcp_now_playing_details_t *update, void *context)
{
    harness_t *harness = context;
    if (update->invalidated)
    {
        ++harness->media_invalidations;
        return;
    }
    ++harness->media_details;
    assert(update->has_duration && update->duration_ms == 180000U);
    assert(update->has_elapsed && update->elapsed_ms == 0U);
    assert(update->has_artwork_transfer_id && update->artwork_transfer_id == 9U);
}

static void capture_route_details(const rtcp_route_details_t *update, void *context)
{
    harness_t *harness = context;
    if (update->invalidated)
    {
        ++harness->route_invalidations;
        return;
    }
    assert(update->maneuver_indices);
    if (++harness->route_details == 1U)
    {
        assert(update->maneuver_index_count == 2U);
        assert(update->maneuver_indices[0] == 8U && update->maneuver_indices[1] == 3U);
        assert(update->has_distance_to_maneuver && update->distance_to_maneuver == 500U);
    }
    else
        assert(update->maneuver_index_count == 0U);
}

static void capture_lane_guidance(const rtcp_lane_guidance_t *update, void *context)
{
    harness_t *harness = context;
    ++harness->lane_guidance;
    assert(update->index == 9U && update->lane_count == 1U);
    assert(update->lanes[0].index == 2U && update->lanes[0].angle_count == 2U);
    assert(update->lanes[0].angles[0] == -90 && update->lanes[0].angles[1] == 0);
}

static const uint8_t now_playing_fixture[] = {
    0,
    24,
    0,
    0,
    0,
    8,
    0,
    1,
    0xe6,
    0x9b,
    0xb2,
    0,
    0,
    6,
    0,
    12,
    'B',
    0,
    0,
    6,
    0,
    6,
    'A',
    0,
    0,
    9,
    0,
    1,
    0,
    5,
    0,
    0,
    1,
};

static const uint8_t route_fixture[] = {
    0,
    6,
    0,
    0,
    0,
    42,
    0,
    5,
    0,
    1,
    1,
    0,
    8,
    0,
    4,
    0xe7,
    0xab,
    0x99,
    0,
    0,
    9,
    0,
    8,
    '2',
    ' ',
    'k',
    'm',
    0,
};

static void capture_route(const rtcp_route_guidance_t *update, void *context)
{
    harness_t *h = context;
    ++h->routes;
    if (update->destination)
        snprintf(h->destination, sizeof(h->destination), "%s", update->destination);
    if (update->distance_remaining)
        snprintf(h->remaining, sizeof(h->remaining), "%s", update->distance_remaining);
    if (update->has_state)
        h->route_state = update->state;
}

static void capture_maneuver(const rtcp_route_maneuver_t *update, void *context)
{
    harness_t *h = context;
    ++h->maneuvers;
    h->maneuver_index = update->index;
    if (update->description)
        snprintf(h->instruction, sizeof(h->instruction), "%s", update->description);
}

static void capture_now_playing(const rtcp_now_playing_t *update, void *context)
{
    harness_t *h = context;
    ++h->now_playing_updates;
    if (update->title)
        snprintf(h->title, sizeof(h->title), "%s", update->title);
    if (update->artist)
        snprintf(h->artist, sizeof(h->artist), "%s", update->artist);
    if (update->album)
        snprintf(h->album, sizeof(h->album), "%s", update->album);
    if (update->has_playing)
        h->playing = update->playing;
}

static void observe_message(bool outgoing, uint16_t id, const uint8_t *body,
                            size_t length, void *context)
{
    harness_t *h = context;
    if (outgoing)
        ++h->observed_tx;
    else
        ++h->observed_rx;
    assert(length <= sizeof(h->observed.body));
    h->observed.id = id;
    h->observed.body_len = length;
    if (length)
        memcpy(h->observed.body, body, length);
}

static bool capture_tx(const uint8_t *bytes, size_t len, void *context)
{
    harness_t *h = context;
    if (len > sizeof(h->tx) - h->tx_len)
        return false;
    memcpy(h->tx + h->tx_len, bytes, len);
    h->tx_len += len;
    return true;
}

static void capture_event(iap2_session_event_t event, void *context)
{
    harness_t *h = context;
    if (h->event_count < sizeof(h->events) / sizeof(h->events[0]))
    {
        h->events[h->event_count++] = (unsigned)event;
    }
}

static bool event_seen(const harness_t *h, iap2_session_event_t event)
{
    for (unsigned i = 0; i < h->event_count; ++i)
    {
        if (h->events[i] == (unsigned)event)
            return true;
    }
    return false;
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static bool find_body_parameter(const uint8_t *body, size_t body_len,
                                uint16_t id, const uint8_t **payload,
                                size_t *payload_len)
{
    size_t offset = 0U;
    while (offset + 4U <= body_len)
    {
        uint16_t length = read_be16(body + offset);
        if (length < 4U || length > body_len - offset)
            return false;
        if (read_be16(body + offset + 2U) == id)
        {
            *payload = body + offset + 4U;
            *payload_len = length - 4U;
            return true;
        }
        offset += length;
    }
    return false;
}

static uint8_t checksum(const uint8_t *bytes, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum = (uint8_t)(sum + bytes[i]);
    return (uint8_t)(0U - sum);
}

static size_t make_peer_frame(uint8_t control, uint8_t seq, uint8_t ack, uint8_t sid,
                              const uint8_t *payload, size_t payload_len,
                              uint8_t *out, size_t capacity)
{
    size_t frame_len = 9 + (payload != NULL ? payload_len + 1 : 0);
    assert(frame_len <= capacity && frame_len <= UINT16_MAX);
    out[0] = 0xff;
    out[1] = 0x5a;
    out[2] = (uint8_t)(frame_len >> 8);
    out[3] = (uint8_t)frame_len;
    out[4] = control;
    out[5] = seq;
    out[6] = ack;
    out[7] = sid;
    out[8] = checksum(out, 8);
    if (payload != NULL)
    {
        memcpy(out + 9, payload, payload_len);
        out[frame_len - 1] = checksum(payload, payload_len);
    }
    return frame_len;
}

static void feed_raw(iap2_session_t *session, const uint8_t *bytes, size_t len,
                     uint64_t now_ms)
{
    iap2_session_feed(session, bytes, len, now_ms);
}

static void feed_peer_frame(iap2_session_t *session, uint8_t control, uint8_t seq,
                            uint8_t ack, uint8_t sid,
                            const uint8_t *payload, size_t payload_len,
                            uint64_t now_ms)
{
    uint8_t frame[2048];
    size_t frame_len = make_peer_frame(control, seq, ack, sid, payload, payload_len,
                                       frame, sizeof(frame));
    feed_raw(session, frame, frame_len, now_ms);
}

static void feed_peer_csm(iap2_session_t *session, uint16_t message_id,
                          const uint8_t *body, size_t body_len, uint64_t now_ms)
{
    uint8_t csm[1606];
    size_t csm_len = body_len + 6U;
    assert(csm_len <= sizeof(csm));
    csm[0] = 0x40;
    csm[1] = 0x40;
    csm[2] = (uint8_t)(csm_len >> 8);
    csm[3] = (uint8_t)csm_len;
    csm[4] = (uint8_t)(message_id >> 8);
    csm[5] = (uint8_t)message_id;
    if (body_len != 0U)
        memcpy(csm + 6, body, body_len);
    feed_peer_frame(session, 0x40, g_harness.peer_seq++, 0, 10, csm, csm_len, now_ms);
}

/* Walk new transmit frames: record CSM payloads and acknowledge each data
 * packet so the core dequeues the next one. */
static void drain_tx(iap2_session_t *session, uint64_t now_ms)
{
    harness_t *h = &g_harness;
    size_t pos = h->scanned;
    if (pos == 0U && h->tx_len >= 6U)
    {
        static const uint8_t marker[] = { 0xff, 0x55, 0x02, 0x00, 0xee, 0x10 };
        if (memcmp(h->tx, marker, sizeof(marker)) == 0)
            pos = sizeof(marker);
    }
    while (pos < h->tx_len)
    {
        if (h->tx_len - pos < 9U)
            break;
        size_t frame_len = ((size_t)h->tx[pos + 2] << 8) | h->tx[pos + 3];
        if (frame_len < 9U || frame_len > h->tx_len - pos)
            break;
        uint8_t control = h->tx[pos + 4];
        uint8_t seq = h->tx[pos + 5];
        const uint8_t *payload = h->tx + pos + 9;
        size_t payload_len = frame_len - 10U;
        if ((control & 0x80U) == 0U && payload_len >= 6U &&
            payload[0] == 0x40U && payload[1] == 0x40U)
        {
            assert(h->message_count < MAX_RECORDED_MESSAGES);
            csm_record_t *record = &h->messages[h->message_count++];
            record->id = (uint16_t)(((uint16_t)payload[4] << 8) | payload[5]);
            record->body_len = payload_len - 6U;
            assert(record->body_len <= sizeof(record->body));
            if (record->body_len != 0U)
            {
                memcpy(record->body, payload + 6, record->body_len);
            }
        }
        pos += frame_len;
        if (payload_len > 0U && (control & 0x80U) == 0U)
        {
            feed_peer_frame(session, 0x40, h->peer_seq, seq, 0, NULL, 0, now_ms);
        }
    }
    h->scanned = pos;
}

static void handshake(iap2_session_t *session)
{
    harness_t *h = &g_harness;
    iap2_session_link_up(session, 0);
    static const uint8_t marker[] = { 0xff, 0x55, 0x02, 0x00, 0xee, 0x10 };
    feed_raw(session, marker, sizeof(marker), 1);
    drain_tx(session, 2);
    h->peer_seq = 30;

    uint8_t sync[19] = {
        1,
        1,
        0x08,
        0x00,
        0x0f,
        0xa0,
        0x01,
        0xf4,
        4,
        1,
        10,
        0,
        1,
        11,
        2,
        1,
        12,
        1,
        2,
    };
    feed_peer_frame(session, 0x80, 30, 0, 0, sync, sizeof(sync), 3);
    drain_tx(session, 4);
    h->peer_seq = 31;
    feed_peer_frame(session, 0x40, 30, 99, 0, NULL, 0, 5);
}

static const uint8_t CERT_BYTES = 0xa0;
static const uint8_t SIG_BYTES = 0x5a;

static int fake_info(struct rtcp_mfi_info *out)
{
    memset(out, 0, sizeof(*out));
    out->id32 = 0x00000200U;
    out->cert_len = 908U;
    out->present = 1;
    return RTCP_OK;
}

static int fake_sign(const uint8_t *challenge, size_t len,
                     uint8_t *sig, size_t sig_cap, size_t *sig_len)
{
    (void)challenge;
    (void)len;
    if (sig_cap < 128U)
        return RTCP_EINVAL;
    memset(sig, SIG_BYTES, 128U);
    *sig_len = 128U;
    return RTCP_OK;
}

static int fake_read_cert(uint8_t *der, size_t cap, size_t *der_len)
{
    size_t len = 908U;
    if (cap < len)
        return RTCP_EINVAL;
    memset(der, CERT_BYTES, len);
    *der_len = len;
    return RTCP_OK;
}

static bool fake_wifi(iap2_session_wifi_t *out, void *context)
{
    (void)context;
    out->ssid = "rtcp-test";
    out->passphrase = "password123";
    out->channel = 6U;
    out->security_type = 2U;
    memset(out->bssid, 0x11, sizeof(out->bssid));
    return true;
}

static bool fake_airplay(iap2_session_airplay_t *out, void *context)
{
    (void)context;
    out->airplay_port = 7000U;
    out->ip_address = "192.168.4.1";
    out->device_identifier = "AA:BB:CC:DD:EE:FF";
    out->public_key = "0123456789abcdef";
    out->source_version = "rt-carplay/0.1";
    out->mdns_ready = true;
    out->event_connected = false;
    return true;
}

/* Wired sessions do not require an mDNS state: the phone learns the
 * endpoint from StartSession and connects over the NCM link directly. */
static bool wired_endpoint_ready = true;
static bool wired_tcp_connected, wired_event_connected;
static bool fake_wired_airplay(iap2_session_airplay_t *out, void *context)
{
    (void)context;
    if (!wired_endpoint_ready)
        return false;
    out->airplay_port = 7100U;
    out->ip_address = "fe80::1234";
    out->device_identifier = "11:22:33:44:55:66";
    out->public_key = "fedcba9876543210";
    out->source_version = "rt-carplay/0.1";
    out->mdns_ready = false;
    out->event_connected = wired_event_connected;
    out->transport_connected = wired_tcp_connected;
    return true;
}

static const csm_record_t *last_message(const harness_t *h, uint16_t id)
{
    for (size_t i = h->message_count; i > 0U; --i)
    {
        if (h->messages[i - 1U].id == id)
            return &h->messages[i - 1U];
    }
    return NULL;
}

int main(void)
{
    harness_t *h = &g_harness;
    memset(h, 0, sizeof(*h));

    static const struct rtcp_mfi mfi = {
        .info = fake_info,
        .sign = fake_sign,
        .read_cert = fake_read_cert,
    };
    iap2_session_config_t config = {
        .identity = {
            .name = "rt-carplay test",
            .model_identifier = "RTCP-TEST",
            .manufacturer = "rt-carplay",
            .serial_number = "001122334455",
            .firmware_version = "0.1",
            .hardware_version = "host",
            .language = "en",
            .wifi_ssid = "rtcp-test",
            .bluetooth_address = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 },
            .provide_location = true,
        },
        .mfi = &mfi,
        .tx = capture_tx,
        .get_wifi = fake_wifi,
        .get_airplay = fake_airplay,
        .on_event = capture_event,
        .on_now_playing = capture_now_playing,
        .on_route_guidance = capture_route,
        .on_route_maneuver = capture_maneuver,
        .context = h,
    };
    iap2_session_t *session = iap2_session_create(&config);
    assert(session != NULL);

    handshake(session);
    drain_tx(session, 6);

    /* Identification request -> IdentificationInformation. */
    feed_peer_csm(session, 0x1d00, NULL, 0, 10);
    drain_tx(session, 11);
    const csm_record_t *identification = last_message(h, 0x1d01);
    assert(identification != NULL && identification->body_len > 0U);
    const uint8_t *component, *flag;
    size_t component_len, flag_len;
    assert(find_body_parameter(identification->body, identification->body_len, 22, &component, &component_len));
    assert(find_body_parameter(component, component_len, 17, &flag, &flag_len) && flag_len == 0U);
    assert(find_body_parameter(component, component_len, 18, &flag, &flag_len) && flag_len == 0U);
    assert(!iap2_session_identification_accepted(session));

    /* Acceptance alone must not send subscriptions before MFi auth. */
    size_t before = h->message_count;
    feed_peer_csm(session, 0x1d02, NULL, 0, 20);
    drain_tx(session, 21);
    assert(h->message_count == before);
    assert(iap2_session_identification_accepted(session));
    assert(event_seen(h, IAP2_SESSION_EVENT_IDENTIFICATION_ACCEPTED));

    /* Certificate exchange. */
    feed_peer_csm(session, 0xaa00, NULL, 0, 30);
    drain_tx(session, 31);
    const csm_record_t *certificate = last_message(h, 0xaa01);
    assert(certificate != NULL && certificate->body_len == 912U);
    assert(certificate->body[0] == 0x03 && certificate->body[1] == 0x90);
    assert(certificate->body[2] == 0 && certificate->body[3] == 0);
    assert(certificate->body[4] == CERT_BYTES);
    assert(event_seen(h, IAP2_SESSION_EVENT_MFI_CERTIFICATE_SENT));

    /* Challenge -> signature response. */
    uint8_t challenge_body[24];
    uint8_t challenge[20];
    memset(challenge, 0x33, sizeof(challenge));
    challenge_body[0] = 0;
    challenge_body[1] = 24;
    challenge_body[2] = 0;
    challenge_body[3] = 0;
    memcpy(challenge_body + 4, challenge, sizeof(challenge));
    feed_peer_csm(session, 0xaa02, challenge_body, sizeof(challenge_body), 40);
    drain_tx(session, 41);
    const csm_record_t *response = last_message(h, 0xaa03);
    assert(response != NULL && response->body_len == 132U);
    assert(response->body[4] == SIG_BYTES);
    assert(!iap2_session_is_authenticated(session));

    feed_peer_csm(session, 0x5001, now_playing_fixture, sizeof(now_playing_fixture), 42);
    const uint8_t location_request[] = { 0, 4, 0, 1, 0, 4, 0, 3 };
    feed_peer_csm(session, 0x5201, route_fixture, sizeof(route_fixture), 42);
    feed_peer_csm(session, IAP2_MSG_START_LOCATION, location_request, sizeof(location_request), 42);
    drain_tx(session, 43);
    assert(h->now_playing_updates == 0);
    assert(h->routes == 0 && iap2_session_location_requested(session) == 0);

    /* Success only becomes valid after the response link ACK. */
    feed_peer_csm(session, 0xaa05, NULL, 0, 50);
    drain_tx(session, 51);
    assert(iap2_session_is_authenticated(session));
    assert(event_seen(h, IAP2_SESSION_EVENT_MFI_AUTHENTICATED));

    /* Subscriptions now flow. */
    assert(last_message(h, 0x5000) != NULL);
    assert(last_message(h, 0x5200) != NULL);
    assert(last_message(h, 0xae00) != NULL);
    assert(last_message(h, 0x4157) != NULL);
    assert(last_message(h, 0x4154) != NULL);
    assert(event_seen(h, IAP2_SESSION_EVENT_SUBSCRIPTIONS_SENT));

    feed_peer_csm(session, 0x5001, now_playing_fixture, sizeof(now_playing_fixture), 52);
    assert(h->now_playing_updates == 1 && h->playing);
    assert(strcmp(h->title, "\xe6\x9b\xb2") == 0 && strcmp(h->artist, "B") == 0 && strcmp(h->album, "A") == 0);
    const uint8_t pause[] = { 0, 9, 0, 1, 0, 5, 0, 0, 2 };
    feed_peer_csm(session, 0x5001, pause, sizeof(pause), 53);
    assert(h->now_playing_updates == 2 && !h->playing && strcmp(h->title, "\xe6\x9b\xb2") == 0);
    const uint8_t bad_utf8[] = { 0, 11, 0, 0, 0, 7, 0, 1, 0xc0, 0xaf, 0 };
    feed_peer_csm(session, 0x5001, bad_utf8, sizeof(bad_utf8), 54);
    feed_peer_csm(session, 0x5001, now_playing_fixture, sizeof(now_playing_fixture) - 1, 55);
    assert(h->now_playing_updates == 2);
    const uint8_t clear_title[] = { 0, 9, 0, 0, 0, 5, 0, 1, 0 };
    feed_peer_csm(session, 0x5001, clear_title, sizeof(clear_title), 56);
    assert(h->now_playing_updates == 3 && h->title[0] == '\0' && strcmp(h->artist, "B") == 0);
    drain_tx(session, 57);

    feed_peer_csm(session, 0x5201, route_fixture, sizeof(route_fixture), 57);
    assert(h->routes == 1 && h->route_state == 1 && strcmp(h->destination, "\xe7\xab\x99") == 0);
    assert(strcmp(h->remaining, "2 km") == 0);
    const uint8_t wrong_display[] = { 0, 6, 0, 0, 0, 43, 0, 5, 0, 1, 0 };
    feed_peer_csm(session, 0x5201, wrong_display, sizeof(wrong_display), 57);
    feed_peer_csm(session, 0x5201, route_fixture, sizeof(route_fixture) - 1U, 57);
    assert(h->routes == 1);
    const uint8_t route_end[] = { 0, 5, 0, 1, 0 };
    feed_peer_csm(session, 0x5201, route_end, sizeof(route_end), 57);
    assert(h->routes == 2 && h->route_state == 0);
    const uint8_t turn[] = { 0, 6, 0, 1, 1, 2, 0, 9, 0, 2, 'L', 'e', 'f', 't', 0 };
    feed_peer_csm(session, 0x5202, turn, sizeof(turn), 57);
    assert(h->maneuvers == 1 && h->maneuver_index == 0x0102 && strcmp(h->instruction, "Left") == 0);
    feed_peer_csm(session, 0x5202, turn + 6, sizeof(turn) - 6, 57);
    assert(h->maneuvers == 1);

    static const char gga[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
    static const char rmc[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
    assert(!iap2_session_send_location(session, gga, sizeof(gga) - 1U, 57));
    feed_peer_csm(session, IAP2_MSG_START_LOCATION, location_request, sizeof(location_request), 57);
    assert(iap2_session_location_requested(session) == RTCP_LOCATION_GGA);
    assert(!iap2_session_send_location(session, rmc, sizeof(rmc) - 1U, 57));
    assert(!iap2_session_send_location(session, gga, sizeof(gga) - 2U, 57));
    char bad_checksum[sizeof(gga)];
    memcpy(bad_checksum, gga, sizeof(gga));
    bad_checksum[sizeof(gga) - 2U] = '0';
    assert(!iap2_session_send_location(session, bad_checksum, sizeof(gga) - 1U, 57));
    assert(iap2_session_send_location(session, gga, sizeof(gga) - 1U, 57));
    drain_tx(session, 58);
    const csm_record_t *location = last_message(h, IAP2_MSG_LOCATION);
    assert(location && location->body_len == 4U + sizeof(gga));
    assert(memcmp(location->body + 4, gga, sizeof(gga)) == 0);
    const uint8_t only_rmc[] = { 0, 4, 0, 2 };
    feed_peer_csm(session, IAP2_MSG_START_LOCATION, only_rmc, sizeof(only_rmc), 58);
    assert(iap2_session_location_requested(session) == RTCP_LOCATION_RMC);
    assert(iap2_session_send_location(session, rmc, sizeof(rmc) - 1U, 58));
    drain_tx(session, 59);
    feed_peer_csm(session, IAP2_MSG_STOP_LOCATION, NULL, 0, 59);
    assert(iap2_session_location_requested(session) == 0);

    /* Pre-transport Wi-Fi configuration. */
    feed_peer_csm(session, 0x5702, NULL, 0, 60);
    drain_tx(session, 61);
    assert(last_message(h, 0x5703) != NULL);
    assert(event_seen(h, IAP2_SESSION_EVENT_WIFI_CONFIG_SENT));

    /* CarPlay availability -> start session invitation. */
    feed_peer_csm(session, 0x4300, NULL, 0, 70);
    assert(event_seen(h, IAP2_SESSION_EVENT_CARPLAY_AVAILABILITY_RECEIVED));
    assert(!event_seen(h, IAP2_SESSION_EVENT_CARPLAY_INVITE_ACKED));
    drain_tx(session, 71);
    const csm_record_t *start = last_message(h, 0x4301);
    assert(start != NULL && start->body_len > 0U);
    assert(event_seen(h, IAP2_SESSION_EVENT_CARPLAY_INVITE_SENT));
    assert(event_seen(h, IAP2_SESSION_EVENT_CARPLAY_INVITE_ACKED));

    /* No second invite while the first one is outstanding. */
    size_t invites = 0U;
    for (size_t i = 0; i < h->message_count; ++i)
    {
        if (h->messages[i].id == 0x4301U)
            ++invites;
    }
    feed_peer_csm(session, 0x4300, NULL, 0, 80);
    drain_tx(session, 81);
    size_t invites_after = 0U;
    for (size_t i = 0; i < h->message_count; ++i)
    {
        if (h->messages[i].id == 0x4301U)
            ++invites_after;
    }
    assert(invites_after == invites);
    assert(!event_seen(h, IAP2_SESSION_EVENT_CARPLAY_REINVITE_SENT));

    /* The invite is re-sent once the pacing interval expires. */
    iap2_session_advance(session, 81U + IAP2_SESSION_CARPLAY_REINVITE_INTERVAL_MS + 1U);
    drain_tx(session, 81U + IAP2_SESSION_CARPLAY_REINVITE_INTERVAL_MS + 2U);
    assert(event_seen(h, IAP2_SESSION_EVENT_CARPLAY_REINVITE_SENT));

    /* An active AirPlay event stream suppresses further invites. */
    iap2_session_notify_airplay_event(session, true, 20000);
    feed_peer_csm(session, 0x4300, NULL, 0, 90);
    drain_tx(session, 91);
    iap2_session_advance(session, 40000);
    drain_tx(session, 40001);

    /* MFi failure is terminal for the authentication state. */
    feed_peer_csm(session, IAP2_MSG_START_LOCATION, location_request, sizeof(location_request), 100);
    feed_peer_csm(session, 0xaa04, NULL, 0, 100);
    drain_tx(session, 101);
    assert(!iap2_session_is_authenticated(session));
    assert(iap2_session_location_requested(session) == 0);
    assert(event_seen(h, IAP2_SESSION_EVENT_MFI_PHONE_REJECTED));
    assert(event_seen(h, IAP2_SESSION_EVENT_MFI_FAILED));

    iap2_session_link_down(session);
    iap2_session_destroy(session);

    /* ------------------------------------------------------------------ */
    /* Wired USB transport                                                 */
    /* ------------------------------------------------------------------ */
    for (unsigned wired_case = 0; wired_case < 5; ++wired_case)
    {
        memset(h, 0, sizeof(*h));
        wired_endpoint_ready = true;
        wired_tcp_connected = wired_event_connected = false;
        iap2_session_config_t wired_config = {
            .identity = {
                .name = "rt-carplay wired",
                .model_identifier = "RTCP-TEST",
                .manufacturer = "rt-carplay",
                .serial_number = "001122334455",
                .firmware_version = "0.1",
                .hardware_version = "host",
                .language = "en",
                .wifi_ssid = NULL,
                .bluetooth_address = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 },
                .provide_location = wired_case == 0,
                .provide_vehicle_speed = wired_case == 0,
            },
            .mfi = &mfi,
            .tx = capture_tx,
            .get_wifi = NULL,
            .get_airplay = fake_wired_airplay,
            .on_event = capture_event,
            .context = h,
            .transport = IAP2_SESSION_TRANSPORT_WIRED_USB,
            .car_play_interface_number = 3U,
            .available_current_ma = 0U,
            .battery_should_charge = false,
            .wired_reinvite_delay_ms = wired_case ? 10000U : 0U,
            .observe_message = observe_message,
            .on_device_time = wired_case == 0 ? capture_device_time : NULL,
            .notify_device_time_invalidation = wired_case == 0,
            .on_phone_status = wired_case == 0 ? capture_phone_status : NULL,
            .on_now_playing_details = wired_case == 0 ? capture_media_details : NULL,
            .on_route_details = wired_case == 0 ? capture_route_details : NULL,
            .on_lane_guidance = wired_case == 0 ? capture_lane_guidance : NULL,
        };
        session = iap2_session_create(&wired_config);
        assert(session != NULL);

        handshake(session);
        drain_tx(session, 6);

        feed_peer_csm(session, 0x1d00, NULL, 0, 10);
        drain_tx(session, 11);
        const csm_record_t *wired_identification = last_message(h, 0x1d01);
        assert(wired_identification != NULL);
        assert(h->observed_rx == 1 && h->observed_tx == 1);
        assert(h->observed.id == 0x1d01 && h->observed.body_len == wired_identification->body_len);
        assert(!memcmp(h->observed.body, wired_identification->body, h->observed.body_len));
        const uint8_t *field = NULL;
        size_t field_len = 0U;
        assert(find_body_parameter(wired_identification->body,
                                   wired_identification->body_len, 8, &field,
                                   &field_len));
        assert(field_len == 1U && field[0] == 2U);
        assert(find_body_parameter(wired_identification->body,
                                   wired_identification->body_len, 7, &field, &field_len));
        bool advertises_time = false;
        bool advertises_lanes = false;
        for (size_t offset = 0; offset + 1U < field_len; offset += 2U)
        {
            advertises_time |= (field[offset] == 0x4e && field[offset + 1U] == 0x0b);
            advertises_lanes |= (field[offset] == 0x52 && field[offset + 1U] == 0x04);
        }
        assert(advertises_time == (wired_case == 0));
        assert(advertises_lanes == (wired_case == 0));
        const uint8_t *usb_group = NULL;
        size_t usb_group_len = 0U;
        assert(find_body_parameter(wired_identification->body,
                                   wired_identification->body_len, 16,
                                   &usb_group, &usb_group_len));
        assert(find_body_parameter(usb_group, usb_group_len, 3, &field,
                                   &field_len));
        assert(field_len == 1U && field[0] == 3U);
        assert(!find_body_parameter(wired_identification->body,
                                    wired_identification->body_len, 17, &field,
                                    &field_len));
        assert(!find_body_parameter(wired_identification->body,
                                    wired_identification->body_len, 24, &field,
                                    &field_len));

        /* Identification and MFi may complete in either order on the wired
     * link; the power update and subscriptions wait for both. */
        feed_peer_csm(session, 0x1d02, NULL, 0, 20);
        drain_tx(session, 21);
        assert(iap2_session_identification_accepted(session));
        assert(last_message(h, 0x5000) == NULL);
        if (wired_case == 0)
        {
            feed_peer_csm(session, IAP2_MSG_DEVICE_TIME_UPDATE,
                          device_time_fixture, sizeof(device_time_fixture), 22);
            drain_tx(session, 23);
            assert(h->device_time_updates == 0);
        }

        feed_peer_csm(session, 0xaa00, NULL, 0, 30);
        drain_tx(session, 31);
        uint8_t wired_challenge_body[24];
        memset(wired_challenge_body, 0x33, sizeof(wired_challenge_body));
        wired_challenge_body[0] = 0;
        wired_challenge_body[1] = 24;
        wired_challenge_body[2] = 0;
        wired_challenge_body[3] = 0;
        feed_peer_csm(session, 0xaa02, wired_challenge_body,
                      sizeof(wired_challenge_body), 40);
        drain_tx(session, 41);
        feed_peer_csm(session, 0xaa05, NULL, 0, 50);
        drain_tx(session, 51);
        assert(iap2_session_is_authenticated(session));

        if (wired_case == 0)
        {
            const uint8_t speed_request[] = { 0, 4, 0, 4 };
            const uint8_t duplicate_speed[] = { 0, 4, 0, 4, 0, 4, 0, 4 };
            const char speed_sentence[] = "$PASCD,1.234,C,R,0,1,0.00,1.250*65\r\n";
            assert(!iap2_session_send_vehicle_speed(session, 1234, RTCP_GEAR_REVERSE, 1250, 52));
            iap2_session_handle_message(session, IAP2_MSG_START_LOCATION, speed_request, sizeof(speed_request), 52);
            assert(iap2_session_location_requested(session) == RTCP_LOCATION_PASCD);
            iap2_session_handle_message(session, IAP2_MSG_START_LOCATION, duplicate_speed, sizeof(duplicate_speed), 52);
            assert(iap2_session_location_requested(session) == RTCP_LOCATION_PASCD);
            assert(!iap2_session_send_vehicle_speed(session, 1234, (rtcp_vehicle_gear_t)'X', 1250, 52));
            assert(iap2_session_send_vehicle_speed(session, 1234, RTCP_GEAR_REVERSE, 1250, 52));
            drain_tx(session, 52);
            const csm_record_t *speed = last_message(h, IAP2_MSG_LOCATION);
            assert(speed && speed->body_len == 4U + sizeof(speed_sentence));
            assert(!memcmp(speed->body + 4, speed_sentence, sizeof(speed_sentence)));
            iap2_session_handle_message(session, IAP2_MSG_STOP_LOCATION, NULL, 0, 52);
            assert(iap2_session_location_requested(session) == 0U);

            const uint8_t battery[] = { 0, 6, 0, 6, 0, 76, 0, 5, 0, 4, 0 };
            const uint8_t cellular[] = { 0, 6, 0, 0, 1, 3, 0, 5, 0, 2, 1 };
            const uint8_t call[] = { 0, 9, 0, 4, 'c', 'a', 'l', 'l', 0, 0, 5, 0, 2, 0 };
            const uint8_t media[] = { 0, 17, 0, 0, 0, 8, 0, 4, 0, 2, 0xbf, 0x20,
                                      0, 5, 0, 26, 9, 0, 12, 0, 1, 0, 8, 0, 1, 0, 0, 0, 0 };
            const uint8_t route[] = { 0, 8, 0, 13, 0, 8, 0, 3, 0, 8, 0, 10, 0, 0, 1, 0xf4 };
            const uint8_t clear_list[] = { 0, 4, 0, 13 };
            const uint8_t route_ended[] = { 0, 5, 0, 1, 0 };
            const uint8_t lane[] = { 0, 6, 0, 1, 0, 9, 0, 22, 0, 2,
                                     0, 6, 0, 0, 0, 2, 0, 6, 0, 2, 0xff, 0xa6, 0, 6, 0, 2, 0, 0 };
            iap2_session_handle_message(session, 0xae01, battery, sizeof(battery) - 1U, 52);
            assert(h->status_updates[RTCP_PHONE_STATUS_POWER] == 0U);
            iap2_session_handle_message(session, 0xae01, battery, sizeof(battery), 52);
            iap2_session_handle_message(session, 0x4158, cellular, sizeof(cellular), 52);
            iap2_session_handle_message(session, 0x4155, call, sizeof(call), 52);
            iap2_session_handle_message(session, 0x5001, media, sizeof(media), 52);
            iap2_session_handle_message(session, 0x5201, route, sizeof(route), 52);
            iap2_session_handle_message(session, IAP2_MSG_LANE_GUIDANCE, lane, sizeof(lane) - 1U, 52);
            iap2_session_handle_message(session, IAP2_MSG_LANE_GUIDANCE, lane, sizeof(lane), 52);
            iap2_session_handle_message(session, 0x5201, clear_list, sizeof(clear_list), 52);
            iap2_session_handle_message(session, 0x5201, route_ended, sizeof(route_ended), 52);
            assert(h->status_updates[RTCP_PHONE_STATUS_POWER] == 1U);
            assert(h->status_updates[RTCP_PHONE_STATUS_CELLULAR] == 1U);
            assert(h->status_updates[RTCP_PHONE_STATUS_CALL] == 1U);
            assert(h->media_details == 1U && h->route_details == 2U && h->route_invalidations == 1U);
            assert(h->lane_guidance == 1U);

            feed_peer_csm(session, IAP2_MSG_DEVICE_TIME_UPDATE,
                          device_time_fixture, sizeof(device_time_fixture), 52);
            drain_tx(session, 53);
            assert(h->device_time_updates == 1);
            assert(h->device_time.has_unix_seconds && h->device_time.unix_seconds == INT64_C(1753400000));
            assert(h->device_time.has_time_zone_offset && h->device_time.time_zone_offset_minutes == -480);
            assert(h->device_time.has_daylight_saving_offset && h->device_time.daylight_saving_offset_minutes == 60);

            const uint8_t offset_only[] = { 0, 6, 0, 1, 1, 74, 0, 5, 0, 2, 0xc4 };
            feed_peer_csm(session, IAP2_MSG_DEVICE_TIME_UPDATE, offset_only, sizeof(offset_only), 54);
            drain_tx(session, 55);
            assert(h->device_time_updates == 2);
            assert(!h->device_time.has_unix_seconds);
            assert(h->device_time.has_time_zone_offset && h->device_time.time_zone_offset_minutes == 330);
            assert(h->device_time.has_daylight_saving_offset && h->device_time.daylight_saving_offset_minutes == -60);

            const uint8_t duplicate[] = { 0, 5, 0, 2, 0, 0, 5, 0, 2, 0 };
            feed_peer_csm(session, IAP2_MSG_DEVICE_TIME_UPDATE, duplicate, sizeof(duplicate), 56);
            drain_tx(session, 57);
            feed_peer_csm(session, IAP2_MSG_DEVICE_TIME_UPDATE,
                          device_time_fixture, sizeof(device_time_fixture) - 1U, 58);
            drain_tx(session, 59);
            assert(h->device_time_updates == 2);
        }
        /* PowerSourceUpdate (0xAE03) precedes the subscription burst. */
        size_t power_index = SIZE_MAX;
        size_t subscription_index = SIZE_MAX;
        for (size_t i = 0; i < h->message_count; ++i)
        {
            if (h->messages[i].id == 0xae03U && power_index == SIZE_MAX)
            {
                power_index = i;
            }
            if (h->messages[i].id == 0x5000U && subscription_index == SIZE_MAX)
            {
                subscription_index = i;
            }
        }
        assert(power_index != SIZE_MAX);
        assert(subscription_index != SIZE_MAX);
        assert(power_index < subscription_index);
        const csm_record_t *power = last_message(h, 0xae03);
        assert(power != NULL && power->body_len == 11U);
        assert(read_be16(power->body) == 6U); /* param 0, u16 */
        assert(power->body[4] == 0U && power->body[5] == 0U);
        assert(read_be16(power->body + 6) == 5U); /* param 1, one byte */
        assert(power->body[10] == 0U);

        /* Wired sessions never send the accessory Wi-Fi configuration. */
        feed_peer_csm(session, 0x4e0e, NULL, 0, 60);
        feed_peer_csm(session, 0x5702, NULL, 0, 61);
        drain_tx(session, 62);
        assert(last_message(h, 0x5703) == NULL);
        assert(!event_seen(h, IAP2_SESSION_EVENT_WIFI_CONFIG_SENT));

        /* Availability can arrive before IPHETH/DAD is ready. Do not advertise
     * an invalid endpoint or require the phone to send availability again. */
        wired_endpoint_ready = false;
        feed_peer_csm(session, 0x4300, NULL, 0, 70);
        drain_tx(session, 71);
        assert(last_message(h, 0x4301) == NULL);
        iap2_session_advance(session, 72);
        assert(last_message(h, 0x4301) == NULL);
        wired_endpoint_ready = true;
        iap2_session_advance(session, 73);
        drain_tx(session, 74);
        /* The deferred wired StartSession still uses group 0 addresses. */
        const csm_record_t *wired_start = last_message(h, 0x4301);
        assert(wired_start != NULL);
        const uint8_t *wired_attributes = NULL;
        size_t wired_attributes_len = 0U;
        assert(find_body_parameter(wired_start->body, wired_start->body_len, 0,
                                   &wired_attributes, &wired_attributes_len));
        assert(find_body_parameter(wired_attributes, wired_attributes_len, 0,
                                   &field, &field_len));
        assert(field_len == sizeof("fe80::1234"));
        assert(memcmp(field, "fe80::1234", field_len) == 0);
        assert(!find_body_parameter(wired_start->body, wired_start->body_len, 1,
                                    &field, &field_len));
        assert(find_body_parameter(wired_start->body, wired_start->body_len, 2,
                                   &field, &field_len));
        assert(field_len == 4U && field[2] == 0x1bU && field[3] == 0xbcU);

        /* Disabled by default; opt-in allows exactly one compatibility retry.
     * Either TCP or event connection suppresses it before SETUP completes. */
        wired_tcp_connected = wired_case == 2;
        wired_event_connected = wired_case == 3;
        wired_endpoint_ready = wired_case != 4;
        iap2_session_advance(session,
                             73U + IAP2_SESSION_CARPLAY_REINVITE_INTERVAL_MS);
        drain_tx(session, 74U + IAP2_SESSION_CARPLAY_REINVITE_INTERVAL_MS);
        size_t wired_invites = 0U;
        for (size_t i = 0; i < h->message_count; ++i)
        {
            if (h->messages[i].id == 0x4301U)
                ++wired_invites;
        }
        assert(wired_invites == (wired_case == 1 ? 2U : 1U));
        assert(event_seen(h, IAP2_SESSION_EVENT_CARPLAY_REINVITE_SENT) == (wired_case == 1));
        if (wired_case == 4)
        {
            wired_endpoint_ready = true;
            iap2_session_advance(session, 10075);
            drain_tx(session, 10076);
            ++wired_invites;
            assert(event_seen(h, IAP2_SESSION_EVENT_CARPLAY_REINVITE_SENT));
        }
        iap2_session_advance(session, 20080);
        drain_tx(session, 20081);
        size_t count = 0;
        for (size_t i = 0; i < h->message_count; ++i)
            count += h->messages[i].id == 0x4301U;
        assert(count == wired_invites); /* Never a periodic retry loop. */

        if (wired_case == 0)
        {
            /* A new phone request is answered even after an earlier send.
         * Temporary endpoint loss must retain this new request as well. */
            wired_endpoint_ready = false;
            feed_peer_csm(session, 0x4300, NULL, 0, 20090);
            drain_tx(session, 20091);
            wired_endpoint_ready = true;
            iap2_session_advance(session, 20092);
            drain_tx(session, 20093);
            count = 0;
            for (size_t i = 0; i < h->message_count; ++i)
                count += h->messages[i].id == 0x4301U;
            assert(count == 2U);
            wired_tcp_connected = true;
            feed_peer_csm(session, 0x4300, NULL, 0, 20100);
            drain_tx(session, 20101);
            count = 0;
            for (size_t i = 0; i < h->message_count; ++i)
                count += h->messages[i].id == 0x4301U;
            assert(count == 2U); /* An accepted TCP must not be interrupted. */
            const uint8_t request[] = { 0, 4, 0, 4 };
            iap2_session_handle_message(session, IAP2_MSG_START_LOCATION, request, sizeof(request), 20102);
            assert(iap2_session_send_vehicle_speed(session, 20102, RTCP_GEAR_PARK, 0, 20102));
        }

        iap2_session_link_down(session);
        size_t bytes_after_close = h->tx_len;
        unsigned messages_after_close = h->observed_rx;
        iap2_session_advance(session, 25000);
        iap2_session_handle_message(session, 0x1d00, NULL, 0, 25000);
        assert(h->tx_len == bytes_after_close && h->observed_rx == messages_after_close);
        assert(iap2_session_location_requested(session) == 0U);
        if (wired_case == 0)
        {
            assert(h->status_updates[RTCP_PHONE_STATUS_INVALIDATED] == 1U);
            assert(h->media_invalidations == 1U && h->route_invalidations == 1U);
            assert(h->device_time_invalidations == 1U);
        }
        iap2_session_destroy(session);
    }

    for (unsigned tunnel = 0; tunnel < 2; ++tunnel)
    {
        memset(h, 0, sizeof(*h));
        config.airplay_tunnel = tunnel != 0;
        config.on_device_time = capture_device_time;
        session = iap2_session_create(&config);
        assert(session != NULL);
        handshake(session);
        drain_tx(session, 6);
        feed_peer_csm(session, 0x1d00, NULL, 0, 10);
        drain_tx(session, 11);
        const csm_record_t *wireless_identity = last_message(h, 0x1d01);
        assert(wireless_identity != NULL);
        const uint8_t *received;
        size_t received_len;
        assert(find_body_parameter(wireless_identity->body, wireless_identity->body_len,
                                   7, &received, &received_len));
        bool advertises_time = false;
        for (size_t offset = 0; offset + 1U < received_len; offset += 2U)
            advertises_time |= (received[offset] == 0x4e && received[offset + 1U] == 0x0b);
        assert(advertises_time == (tunnel != 0));
        iap2_session_destroy(session);
    }

    puts("iap2 session host test passed");
    return 0;
}
