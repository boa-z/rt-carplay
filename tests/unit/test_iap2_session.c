/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iap2_session.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

#define TX_CAPACITY 65536U
#define MAX_RECORDED_MESSAGES 64U

typedef struct {
    uint16_t id;
    uint8_t body[1600];
    size_t body_len;
} csm_record_t;

typedef struct {
    uint8_t tx[TX_CAPACITY];
    size_t tx_len;
    size_t scanned;
    uint8_t peer_seq;
    csm_record_t messages[MAX_RECORDED_MESSAGES];
    size_t message_count;
    unsigned events[16];
    unsigned event_count;
} harness_t;

static harness_t g_harness;

static bool capture_tx(const uint8_t *bytes, size_t len, void *context)
{
    harness_t *h = context;
    if (len > sizeof(h->tx) - h->tx_len) return false;
    memcpy(h->tx + h->tx_len, bytes, len);
    h->tx_len += len;
    return true;
}

static void capture_event(iap2_session_event_t event, void *context)
{
    harness_t *h = context;
    if (h->event_count < sizeof(h->events) / sizeof(h->events[0])) {
        h->events[h->event_count++] = (unsigned)event;
    }
}

static bool event_seen(const harness_t *h, iap2_session_event_t event)
{
    for (unsigned i = 0; i < h->event_count; ++i) {
        if (h->events[i] == (unsigned)event) return true;
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
    if (payload != NULL) {
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
    if (body_len != 0U) memcpy(csm + 6, body, body_len);
    feed_peer_frame(session, 0x40, g_harness.peer_seq++, 0, 10, csm, csm_len, now_ms);
}

/* Walk new transmit frames: record CSM payloads and acknowledge each data
 * packet so the core dequeues the next one. */
static void drain_tx(iap2_session_t *session, uint64_t now_ms)
{
    harness_t *h = &g_harness;
    size_t pos = h->scanned;
    if (pos == 0U && h->tx_len >= 6U) {
        static const uint8_t marker[] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
        if (memcmp(h->tx, marker, sizeof(marker)) == 0) pos = sizeof(marker);
    }
    while (pos < h->tx_len) {
        if (h->tx_len - pos < 9U) break;
        size_t frame_len = ((size_t)h->tx[pos + 2] << 8) | h->tx[pos + 3];
        if (frame_len < 9U || frame_len > h->tx_len - pos) break;
        uint8_t control = h->tx[pos + 4];
        uint8_t seq = h->tx[pos + 5];
        const uint8_t *payload = h->tx + pos + 9;
        size_t payload_len = frame_len - 10U;
        if ((control & 0x80U) == 0U && payload_len >= 6U &&
            payload[0] == 0x40U && payload[1] == 0x40U) {
            assert(h->message_count < MAX_RECORDED_MESSAGES);
            csm_record_t *record = &h->messages[h->message_count++];
            record->id = (uint16_t)(((uint16_t)payload[4] << 8) | payload[5]);
            record->body_len = payload_len - 6U;
            assert(record->body_len <= sizeof(record->body));
            if (record->body_len != 0U) {
                memcpy(record->body, payload + 6, record->body_len);
            }
        }
        pos += frame_len;
        if (payload_len > 0U && (control & 0x80U) == 0U) {
            feed_peer_frame(session, 0x40, h->peer_seq, seq, 0, NULL, 0, now_ms);
        }
    }
    h->scanned = pos;
}

static void handshake(iap2_session_t *session)
{
    harness_t *h = &g_harness;
    iap2_session_link_up(session, 0);
    static const uint8_t marker[] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
    feed_raw(session, marker, sizeof(marker), 1);
    drain_tx(session, 2);
    h->peer_seq = 30;

    uint8_t sync[19] = {
        1, 1, 0x08, 0x00, 0x0f, 0xa0, 0x01, 0xf4, 4, 1,
        10, 0, 1, 11, 2, 1, 12, 1, 2,
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
    if (sig_cap < 128U) return RTCP_EINVAL;
    memset(sig, SIG_BYTES, 128U);
    *sig_len = 128U;
    return RTCP_OK;
}

static int fake_read_cert(uint8_t *der, size_t cap, size_t *der_len)
{
    size_t len = 908U;
    if (cap < len) return RTCP_EINVAL;
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

static const csm_record_t *last_message(const harness_t *h, uint16_t id)
{
    for (size_t i = h->message_count; i > 0U; --i) {
        if (h->messages[i - 1U].id == id) return &h->messages[i - 1U];
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
            .bluetooth_address = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55},
        },
        .mfi = &mfi,
        .tx = capture_tx,
        .get_wifi = fake_wifi,
        .get_airplay = fake_airplay,
        .on_event = capture_event,
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

    /* Pre-transport Wi-Fi configuration. */
    feed_peer_csm(session, 0x5702, NULL, 0, 60);
    drain_tx(session, 61);
    assert(last_message(h, 0x5703) != NULL);
    assert(event_seen(h, IAP2_SESSION_EVENT_WIFI_CONFIG_SENT));

    /* CarPlay availability -> start session invitation. */
    feed_peer_csm(session, 0x4300, NULL, 0, 70);
    drain_tx(session, 71);
    const csm_record_t *start = last_message(h, 0x4301);
    assert(start != NULL && start->body_len > 0U);
    assert(event_seen(h, IAP2_SESSION_EVENT_CARPLAY_INVITE_SENT));

    /* No second invite while the first one is outstanding. */
    size_t invites = 0U;
    for (size_t i = 0; i < h->message_count; ++i) {
        if (h->messages[i].id == 0x4301U) ++invites;
    }
    feed_peer_csm(session, 0x4300, NULL, 0, 80);
    drain_tx(session, 81);
    size_t invites_after = 0U;
    for (size_t i = 0; i < h->message_count; ++i) {
        if (h->messages[i].id == 0x4301U) ++invites_after;
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
    feed_peer_csm(session, 0xaa04, NULL, 0, 100);
    drain_tx(session, 101);
    assert(!iap2_session_is_authenticated(session));
    assert(event_seen(h, IAP2_SESSION_EVENT_MFI_FAILED));

    iap2_session_link_down(session);
    iap2_session_destroy(session);
    puts("iap2 session host test passed");
    return 0;
}
