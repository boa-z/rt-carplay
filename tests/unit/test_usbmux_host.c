/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usbmux_host.h"
#include "lockdown.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"
#include "rtcp/usbmux.h"

#define FAKE_SENT_MAX 16U
#define FAKE_RX_MAX 8U

typedef struct {
    uint8_t data[RTCP_USBMUX_MAX_PAYLOAD + 64U];
    size_t length;
} fake_packet_t;

typedef struct {
    fake_packet_t sent[FAKE_SENT_MAX];
    unsigned sent_count;
    fake_packet_t received[FAKE_RX_MAX];
    unsigned received_count;
    unsigned received_read;
    uint32_t now_ms;
} fake_transport_t;

static int fake_send(void *user, const uint8_t *data, size_t length)
{
    fake_transport_t *fake = user;
    assert(fake->sent_count < FAKE_SENT_MAX);
    assert(length <= sizeof(fake->sent[0].data));
    memcpy(fake->sent[fake->sent_count].data, data, length);
    fake->sent[fake->sent_count].length = length;
    ++fake->sent_count;
    return 0;
}

static int fake_recv(void *user, uint8_t *data, size_t capacity,
                     uint32_t timeout_ms)
{
    fake_transport_t *fake = user;
    if (fake->received_read >= fake->received_count) {
        fake->now_ms += timeout_ms;
        return RTCP_ETIMEDOUT;
    }
    ++fake->now_ms;
    const fake_packet_t *packet = &fake->received[fake->received_read++];
    assert(packet->length <= capacity);
    memcpy(data, packet->data, packet->length);
    return (int)packet->length;
}

static void queue_device_frame(fake_transport_t *fake, uint16_t source_port,
                               uint16_t destination_port, uint32_t sequence,
                               uint32_t acknowledgment, uint8_t flags,
                               const void *payload, size_t payload_length)
{
    assert(fake->received_count < FAKE_RX_MAX);
    fake_packet_t *packet = &fake->received[fake->received_count++];
    packet->length = rtcp_usbmux_build_tcp(
        packet->data, sizeof(packet->data), 0U, source_port, destination_port,
        sequence, acknowledgment, flags, payload, payload_length);
    assert(packet->length != 0U);
}

static rtcp_usbmux_tcp_packet_t sent_tcp(const fake_transport_t *fake,
                                         unsigned index)
{
    assert(index < fake->sent_count);
    size_t frame_length = 0U;
    size_t payload_length = 0U;
    uint32_t proto = 0U;
    const uint8_t *payload = NULL;
    assert(rtcp_usbmux_parse_frame(fake->sent[index].data,
                                   fake->sent[index].length, &frame_length,
                                   &proto, &payload, &payload_length) == 1);
    assert(frame_length == fake->sent[index].length);
    assert(proto == RTCP_USBMUX_PROTO_TCP);
    rtcp_usbmux_tcp_packet_t packet;
    assert(rtcp_usbmux_parse_tcp(payload, payload_length, &packet) == RTCP_OK);
    return packet;
}

static void start_host(rtcp_usbmux_host_t *host, fake_transport_t *fake)
{
    memset(fake, 0, sizeof(*fake));
    rtcp_usbmux_transport_t transport = {
        .send = fake_send,
        .recv = fake_recv,
        .user = fake,
    };
    rtcp_usbmux_init(host, &transport);
    fake->received[0].length = rtcp_usbmux_build_version(
        fake->received[0].data, sizeof(fake->received[0].data));
    fake->received_count = 1U;
    assert(rtcp_usbmux_start(host, 0U) == RTCP_OK);
    /* Version packet, then setup. */
    assert(fake->sent_count == 2U);
    assert(fake->sent[0].length == RTCP_USBMUX_VERSION_PACKET_SIZE);
    assert(fake->sent[0].data[8] == 0U && fake->sent[0].data[11] == 2U);
    assert(fake->sent[1].length == 17U);
    assert(fake->sent[1].data[16] == 0x07U);
    assert(fake->sent[1].data[12] == 0U && fake->sent[1].data[13] == 0U);
    assert(fake->sent[1].data[14] == 0xffU && fake->sent[1].data[15] == 0xffU);
}

static void test_connect_and_stream(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    start_host(&host, &fake);

    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 100U, 1U,
                       RTCP_USBMUX_TH_SYN | RTCP_USBMUX_TH_ACK, NULL, 0U);

    rtcp_usbmux_connection_t *connection = NULL;
    assert(rtcp_usbmux_connect(&host, RTCP_USBMUX_LOCKDOWN_PORT, 0U, 1000U,
                               &connection) == RTCP_OK);
    assert(connection != NULL && connection->established);
    assert(connection->local_port == 1U);
    assert(connection->tx_sequence == 1U);
    assert(connection->tx_acknowledgment == 101U);

    /* The SYN was sent before the SYN|ACK, and the handshake acknowledged
     * it with our new sequence number. */
    rtcp_usbmux_tcp_packet_t syn = sent_tcp(&fake, 2U);
    assert(fake.sent[2].data[12] == 0U && fake.sent[2].data[13] == 1U);
    assert(fake.sent[2].data[14] == 0xffU && fake.sent[2].data[15] == 0xffU);
    assert(syn.source_port == 1U);
    assert(syn.destination_port == RTCP_USBMUX_LOCKDOWN_PORT);
    assert(syn.sequence == 0U);
    assert(syn.flags == RTCP_USBMUX_TH_SYN);
    rtcp_usbmux_tcp_packet_t ack = sent_tcp(&fake, 3U);
    assert(ack.sequence == 1U);
    assert(ack.acknowledgment == 101U);
    assert(ack.flags == RTCP_USBMUX_TH_ACK);
    assert(ack.payload_length == 0U);

    static const char hello[] = "hello";
    assert(rtcp_usbmux_connection_send(&host, connection, hello,
                                       sizeof(hello) - 1U) == RTCP_OK);
    rtcp_usbmux_tcp_packet_t data = sent_tcp(&fake, 4U);
    assert(data.sequence == 1U && data.acknowledgment == 101U);
    assert(data.payload_length == sizeof(hello) - 1U);
    assert(memcmp(data.payload, hello, sizeof(hello) - 1U) == 0);

    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 101U, 6U,
                       RTCP_USBMUX_TH_ACK, "world", 5U);
    assert(rtcp_usbmux_pump(&host, 0U, 0U) == 1);
    char buffer[16] = {0};
    assert(rtcp_usbmux_connection_recv(&host, connection, buffer,
                                       sizeof(buffer)) == 5);
    assert(memcmp(buffer, "world", 5U) == 0);
    ack = sent_tcp(&fake, 5U);
    assert(ack.sequence == 6U && ack.acknowledgment == 106U);

    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 106U, 6U,
                       RTCP_USBMUX_TH_FIN | RTCP_USBMUX_TH_ACK, NULL, 0U);
    assert(rtcp_usbmux_pump(&host, 0U, 0U) == 1);
    assert(rtcp_usbmux_connection_recv(&host, connection, buffer,
                                       sizeof(buffer)) == RTCP_ESTATE);
    rtcp_usbmux_connection_close(&host, connection);
}

static void test_connect_reset(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    start_host(&host, &fake);

    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 100U, 1U,
                       RTCP_USBMUX_TH_RST, NULL, 0U);
    rtcp_usbmux_connection_t *connection = NULL;
    assert(rtcp_usbmux_connect(&host, RTCP_USBMUX_LOCKDOWN_PORT, 0U, 500U,
                               &connection) == RTCP_EIO);
    assert(connection == NULL);
}

static void test_connect_timeout(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    start_host(&host, &fake);

    rtcp_usbmux_connection_t *connection = NULL;
    assert(rtcp_usbmux_connect(&host, RTCP_USBMUX_LOCKDOWN_PORT, 0U, 10U,
                               &connection) == RTCP_ETIMEDOUT);
    /* The slot must be reusable after a failed connect; the next stream uses
     * the next local port. */
    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 2U, 100U, 1U,
                       RTCP_USBMUX_TH_SYN | RTCP_USBMUX_TH_ACK, NULL, 0U);
    assert(rtcp_usbmux_connect(&host, RTCP_USBMUX_LOCKDOWN_PORT, 0U, 500U,
                               &connection) == RTCP_OK);
    rtcp_usbmux_connection_close(&host, connection);
}

static void test_desynchronised_stream(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    start_host(&host, &fake);

    fake_packet_t *packet = &fake.received[fake.received_count++];
    memset(packet->data, 0, sizeof(packet->data));
    packet->data[3] = 0x7f; /* unsupported protocol */
    packet->data[7] = 36U;
    packet->length = 36U;
    assert(rtcp_usbmux_pump(&host, 0U, 0U) == RTCP_EIO);
}

static void test_two_streams(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    start_host(&host, &fake);

    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 100U, 1U,
                       RTCP_USBMUX_TH_SYN | RTCP_USBMUX_TH_ACK, NULL, 0U);
    rtcp_usbmux_connection_t *first = NULL;
    assert(rtcp_usbmux_connect(&host, RTCP_USBMUX_LOCKDOWN_PORT, 0U, 500U,
                               &first) == RTCP_OK);

    queue_device_frame(&fake, 50000U, 2U, 200U, 1U,
                       RTCP_USBMUX_TH_SYN | RTCP_USBMUX_TH_ACK, NULL, 0U);
    rtcp_usbmux_connection_t *second = NULL;
    assert(rtcp_usbmux_connect(&host, 50000U, 0U, 500U, &second) == RTCP_OK);
    assert(second->local_port == 2U);

    /* A frame on the second stream must not reach the first one. */
    queue_device_frame(&fake, 50000U, 2U, 201U, 1U, RTCP_USBMUX_TH_ACK,
                       "abc", 3U);
    assert(rtcp_usbmux_pump(&host, 0U, 0U) == 1);
    char buffer[8] = {0};
    assert(rtcp_usbmux_connection_recv(&host, first, buffer,
                                       sizeof(buffer)) == 0);
    assert(rtcp_usbmux_connection_recv(&host, second, buffer,
                                       sizeof(buffer)) == 3);
    assert(memcmp(buffer, "abc", 3U) == 0);

    rtcp_usbmux_connection_close(&host, first);
    rtcp_usbmux_connection_close(&host, second);
}

static void test_large_tls_record(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    start_host(&host, &fake);

    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 100U, 1U,
                       RTCP_USBMUX_TH_SYN | RTCP_USBMUX_TH_ACK, NULL, 0U);
    rtcp_usbmux_connection_t *connection = NULL;
    assert(rtcp_usbmux_connect(&host, RTCP_USBMUX_LOCKDOWN_PORT, 0U, 500U,
                               &connection) == RTCP_OK);

    /* One frame carrying a full-size record: the old 4 KiB connection ring
     * reset the stream here.  The payload also exactly fills the ring. */
    static uint8_t payload[RTCP_USBMUX_MAX_PAYLOAD];
    for (size_t i = 0U; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i * 7U);
    }
    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 101U, 1U,
                       RTCP_USBMUX_TH_ACK, payload, sizeof(payload));
    assert(rtcp_usbmux_pump(&host, 0U, 0U) == 1);

    static uint8_t received[RTCP_USBMUX_MAX_PAYLOAD];
    assert(rtcp_usbmux_connection_recv(&host, connection, received, 100U) == 100);
    assert(memcmp(received, payload, 100U) == 0);
    /* Partial receive: the rest is still buffered and drains in one call. */
    int rest = rtcp_usbmux_connection_recv(
        &host, connection, received + 100U, sizeof(received) - 100U);
    assert(rest == (int)sizeof(payload) - 100);
    assert(memcmp(received + 100U, payload + 100U,
                  sizeof(payload) - 100U) == 0);
    assert(rtcp_usbmux_connection_recv(&host, connection, received,
                                       sizeof(received)) == 0);

    rtcp_usbmux_connection_close(&host, connection);
}

static void test_back_to_back_frames(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    start_host(&host, &fake);

    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 100U, 1U,
                       RTCP_USBMUX_TH_SYN | RTCP_USBMUX_TH_ACK, NULL, 0U);
    rtcp_usbmux_connection_t *connection = NULL;
    assert(rtcp_usbmux_connect(&host, RTCP_USBMUX_LOCKDOWN_PORT, 0U, 500U,
                               &connection) == RTCP_OK);

    /* Two complete mux frames in one bulk transfer. */
    fake_packet_t *packet = &fake.received[fake.received_count++];
    size_t offset = rtcp_usbmux_build_tcp(
        packet->data, sizeof(packet->data), 0U, RTCP_USBMUX_LOCKDOWN_PORT, 1U,
        101U, 1U, RTCP_USBMUX_TH_ACK, "one", 3U);
    assert(offset != 0U);
    offset += rtcp_usbmux_build_tcp(
        packet->data + offset, sizeof(packet->data) - offset, 1U,
        RTCP_USBMUX_LOCKDOWN_PORT, 1U, 104U, 1U, RTCP_USBMUX_TH_ACK, "two",
        3U);
    packet->length = offset;

    assert(rtcp_usbmux_pump(&host, 0U, 0U) == 2);
    char buffer[8] = {0};
    assert(rtcp_usbmux_connection_recv(&host, connection, buffer,
                                       sizeof(buffer)) == 6);
    assert(memcmp(buffer, "onetwo", 6U) == 0);

    rtcp_usbmux_connection_close(&host, connection);
}

static void test_partial_frame(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    start_host(&host, &fake);

    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 100U, 1U,
                       RTCP_USBMUX_TH_SYN | RTCP_USBMUX_TH_ACK, NULL, 0U);
    rtcp_usbmux_connection_t *connection = NULL;
    assert(rtcp_usbmux_connect(&host, RTCP_USBMUX_LOCKDOWN_PORT, 0U, 500U,
                               &connection) == RTCP_OK);

    static uint8_t frame[128];
    size_t frame_length = rtcp_usbmux_build_tcp(
        frame, sizeof(frame), 0U, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 101U, 1U,
        RTCP_USBMUX_TH_ACK, "split frame", 11U);
    assert(frame_length != 0U);

    fake_packet_t *first = &fake.received[fake.received_count++];
    memcpy(first->data, frame, 20U);
    first->length = 20U;
    fake_packet_t *second = &fake.received[fake.received_count++];
    memcpy(second->data, frame + 20U, frame_length - 20U);
    second->length = frame_length - 20U;

    assert(rtcp_usbmux_pump(&host, 0U, 0U) == 0);
    char buffer[16] = {0};
    assert(rtcp_usbmux_connection_recv(&host, connection, buffer,
                                       sizeof(buffer)) == 0);
    assert(rtcp_usbmux_pump(&host, 0U, 0U) == 1);
    assert(rtcp_usbmux_connection_recv(&host, connection, buffer,
                                       sizeof(buffer)) == 11);
    assert(memcmp(buffer, "split frame", 11U) == 0);

    rtcp_usbmux_connection_close(&host, connection);
}

static void test_overflow_resets_and_reconnect(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    static uint8_t payload[RTCP_USBMUX_MAX_PAYLOAD / 2U + 1000U];
    start_host(&host, &fake);
    memset(payload, 0xa5, sizeof(payload));

    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 100U, 1U,
                       RTCP_USBMUX_TH_SYN | RTCP_USBMUX_TH_ACK, NULL, 0U);
    rtcp_usbmux_connection_t *connection = NULL;
    assert(rtcp_usbmux_connect(&host, RTCP_USBMUX_LOCKDOWN_PORT, 0U, 500U,
                               &connection) == RTCP_OK);

    /* Two frames whose combined payloads exceed the ring while the owner
     * has not drained between pumps: the stream must reset, not corrupt. */
    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 101U, 1U,
                       RTCP_USBMUX_TH_ACK, payload, sizeof(payload));
    assert(rtcp_usbmux_pump(&host, 0U, 0U) == 1);
    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U,
                       101U + (uint32_t)sizeof(payload), 1U,
                       RTCP_USBMUX_TH_ACK, payload, sizeof(payload));
    assert(rtcp_usbmux_pump(&host, 0U, 0U) == 1);
    assert(connection->reset);
    assert(connection->remote_closed);
    rtcp_usbmux_tcp_packet_t rst = sent_tcp(&fake, fake.sent_count - 1U);
    assert((rst.flags & RTCP_USBMUX_TH_RST) != 0U);

    static uint8_t drained[RTCP_USBMUX_MAX_PAYLOAD];
    assert(rtcp_usbmux_connection_recv(&host, connection, drained,
                                       sizeof(drained)) ==
           (int)sizeof(payload));
    assert(rtcp_usbmux_connection_recv(&host, connection, drained,
                                       sizeof(drained)) == RTCP_EIO);
    rtcp_usbmux_connection_close(&host, connection);

    /* The slot and local port are reusable after the reset. */
    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 2U, 300U, 1U,
                       RTCP_USBMUX_TH_SYN | RTCP_USBMUX_TH_ACK, NULL, 0U);
    connection = NULL;
    assert(rtcp_usbmux_connect(&host, RTCP_USBMUX_LOCKDOWN_PORT, 0U, 500U,
                               &connection) == RTCP_OK);
    assert(connection->local_port == 2U);
    rtcp_usbmux_connection_close(&host, connection);
}

/* Fixed source-derived VERSION vectors: 8-byte mux header + 12-byte body. */
static void test_version_negotiation(void)
{
    static const uint8_t version[] = {
        0,0,0,0, 0,0,0,20, 0,0,0,2, 0,0,0,0, 0,0,0,0
    };
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    rtcp_usbmux_transport_t transport = {fake_send, fake_recv, &fake};
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        memset(&fake, 0, sizeof(fake));
        rtcp_usbmux_init(&host, &transport);
        if (scenario != 0) {
            memcpy(fake.received[0].data, version, sizeof(version));
            fake.received[0].length = sizeof(version);
            fake.received_count = 1;
        }
        if (scenario == 1) { /* fragmented VERSION */
            fake.received[0].length = 7;
            memcpy(fake.received[1].data, version + 7, sizeof(version) - 7);
            fake.received[1].length = sizeof(version) - 7;
            fake.received_count = 2;
        } else if (scenario == 2) { /* v1 is explicitly unsupported */
            fake.received[0].data[11] = 1;
        } else if (scenario == 3) { /* missing last byte must not succeed */
            fake.received[0].length--;
        } else if (scenario == 4) { /* malformed declared size */
            fake.received[0].data[7] = 16;
        }
        int ret = rtcp_usbmux_start(&host, 0);
        if (scenario == 1) {
            assert(ret == RTCP_OK && host.started);
            assert(host.peer_major == 2 && host.peer_minor == 0);
            assert(host.rx_bytes == 20 && host.rx_frames == 1);
            assert(fake.sent_count == 2);
            unsigned sent = fake.sent_count;
            assert(rtcp_usbmux_start(&host, 0) == RTCP_OK);
            assert(fake.sent_count == sent);
        } else {
            assert(ret == (scenario == 2 || scenario == 4 ? RTCP_EIO : RTCP_ETIMEDOUT));
            assert(!host.started && fake.sent_count == 1); /* no SETUP */
            rtcp_usbmux_connection_t *connection = NULL;
            assert(rtcp_usbmux_connect(&host, 62078, 0, 100, &connection) == RTCP_ESTATE);
        }
    }
}

static void test_mux_sequence_echo(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    start_host(&host, &fake);
    queue_device_frame(&fake, 62078, 1, 100, 1,
                       RTCP_USBMUX_TH_SYN | RTCP_USBMUX_TH_ACK, NULL, 0);
    /* Different tx/rx sentinels ensure we echo rx_seq, as usbmuxd does. */
    uint8_t *header = fake.received[fake.received_count - 1].data;
    header[12] = 0xab; header[13] = 0xcd;
    header[14] = 0x12; header[15] = 0x34;
    rtcp_usbmux_connection_t *connection = NULL;
    assert(rtcp_usbmux_connect(&host, 62078, 0, 100, &connection) == RTCP_OK);
    assert(fake.sent[3].data[13] == 2); /* SETUP=0, SYN=1, ACK=2 */
    assert(fake.sent[3].data[14] == 0x12 && fake.sent[3].data[15] == 0x34);
    rtcp_usbmux_connection_close(&host, connection);
}

static void test_peer_header_fields(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    const uint32_t fields[] = {0, 0x12345678, RTCP_USBMUX_MAGIC};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        start_host(&host, &fake);
        /* Independent 36-byte peer vector, not generated by our TX builder. */
        static const uint8_t synack[] = {
            0,0,0,6, 0,0,0,36, 0,0,0,0, 0,0,0,1,
            0xf2,0x7e,0,1, 0,0,0,100, 0,0,0,1,
            0x50,0x12,2,0, 0,0,0,0
        };
        fake_packet_t *reply = &fake.received[fake.received_count++];
        memcpy(reply->data, synack, sizeof(synack)); reply->length = sizeof(synack);
        uint8_t *p = reply->data;
        for (unsigned j = 0; j < 4; ++j) p[8+j] = (uint8_t)(fields[i] >> (24-8*j));
        rtcp_usbmux_connection_t *connection = NULL;
        assert(rtcp_usbmux_connect(&host, 62078, 0, 100, &connection) == RTCP_OK);
        assert(host.rx_bytes == 56 && host.rx_frames == 2);
        rtcp_usbmux_connection_close(&host, connection);
    }
    start_host(&host, &fake);
    fake_packet_t *packet = &fake.received[fake.received_count++];
    static const uint8_t control[] = {
        0,0,0,1, 0,0,0,17, 0,0,0,0, 0,0,0,0, 7
    };
    memcpy(packet->data, control, sizeof(control)); packet->length = sizeof(control);
    assert(rtcp_usbmux_pump(&host, 100, 0) == 1);
    assert(host.rx_frames == 2); /* control is not delivered as TCP */
}

static uint32_t fake_clock(void *user)
{
    return ((fake_transport_t *)user)->now_ms;
}

struct stream_bridge {
    rtcp_usbmux_host_t *host;
    rtcp_usbmux_connection_t *connection;
    fake_transport_t *fake;
};
static int bridge_read(void *context, void *data, size_t n, uint32_t timeout_ms)
{
    struct stream_bridge *b = context;
    return rtcp_usbmux_connection_read_wait(b->host, b->connection, data, n,
                                           timeout_ms, fake_clock, b->fake);
}
static int bridge_write(void *context, const void *data, size_t n)
{
    struct stream_bridge *b = context;
    return rtcp_usbmux_connection_send(b->host, b->connection, data, n);
}

static void test_framed_exchange_and_stream_wait(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    start_host(&host, &fake);
    queue_device_frame(&fake, 62078, 1, 0, 1,
                       RTCP_USBMUX_TH_SYN | RTCP_USBMUX_TH_ACK, NULL, 0);
    rtcp_usbmux_connection_t *connection = NULL;
    assert(rtcp_usbmux_connect(&host, 62078, 0, 100, &connection) == RTCP_OK);
    struct stream_bridge bridge = {&host, connection, &fake};
    static const rtcp_stream_ops_t ops = {bridge_read, bridge_write};
    rtcp_stream_t stream = {&ops, &bridge};
    /* ACK-only traffic and a warning precede a split length-prefixed reply. */
    queue_device_frame(&fake, 62078, 1, 1, 1, RTCP_USBMUX_TH_ACK, NULL, 0);
    static const uint8_t control[] = {
        0,0,0,1, 0,0,0,19, 0xfa,0xce,0xfa,0xce, 0,1,0,1, 5,'o','k'
    };
    fake_packet_t *p = &fake.received[fake.received_count++];
    memcpy(p->data, control, sizeof(control)); p->length = sizeof(control);
    static const char xml[] = "<plist><dict><key>Value</key><string>test</string></dict></plist>";
    uint8_t framed[4 + sizeof(xml) - 1] = {0,0,0,sizeof(xml)-1};
    memcpy(framed + 4, xml, sizeof(xml) - 1);
    queue_device_frame(&fake, 62078, 1, 1, 1, RTCP_USBMUX_TH_ACK, framed, 2);
    queue_device_frame(&fake, 62078, 1, 3, 1, RTCP_USBMUX_TH_ACK,
                       framed + 2, sizeof(framed) - 2);
    uint8_t request[1024], reply[256]; size_t reply_length = 0;
    size_t n = rtcp_lockdown_build_get_value(request, sizeof(request), "rt-carplay", "ProductVersion");
    assert(n && n < 65536);
    unsigned first = fake.sent_count;
    assert(rtcp_lockdown_exchange(&stream, request, n, reply, sizeof(reply),
                                   &reply_length, 100) == RTCP_OK);
    assert(reply_length == sizeof(xml)-1 && !memcmp(reply, xml, reply_length));
    rtcp_usbmux_tcp_packet_t prefix = sent_tcp(&fake, first);
    assert(prefix.payload_length == 4 && prefix.payload[0] == 0 && prefix.payload[1] == 0);
    assert(((size_t)prefix.payload[2] << 8 | prefix.payload[3]) == n);
    rtcp_usbmux_tcp_packet_t body = sent_tcp(&fake, first + 1);
    assert(body.payload_length == n && !memcmp(body.payload, request, n));
    assert(body.sequence == prefix.sequence + 4);
    assert(host.control_type == 5 && !strcmp(host.control_text, "ok"));
    /* No-data wait respects the clock, including unsigned tick wrap. */
    fake.now_ms = UINT32_MAX - 20U;
    assert(bridge_read(&bridge, reply, sizeof(reply), 50) == RTCP_ETIMEDOUT);
    assert(fake.now_ms == 29U);
    rtcp_usbmux_connection_close(&host, connection);
}

static void test_control_diagnostic_bounds(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    start_host(&host, &fake);
    fake_packet_t *p = &fake.received[fake.received_count++];
    memset(p->data, 'x', 117);
    static const uint8_t header[17] = {0,0,0,1, 0,0,0,117, 0,0,0,0, 0,0,0,0, 3};
    memcpy(p->data, header, sizeof(header)); p->data[18] = '\n';
    p->length = 117;
    assert(rtcp_usbmux_pump(&host, 100, 0) == 1);
    assert(host.control_count == 1 && host.control_type == 3);
    assert(strlen(host.control_text) == 64 && host.control_text[1] == '.');
    /* An empty CONTROL clears stale diagnostic text without dereferencing it. */
    p = &fake.received[fake.received_count++];
    memcpy(p->data, header, 16); p->data[7] = 16; p->length = 16;
    assert(rtcp_usbmux_pump(&host, 100, 0) == 1);
    assert(host.control_type == 0 && host.control_text[0] == 0);
}

static void test_optional_reply_padding(void)
{
    static rtcp_usbmux_host_t host;
    static fake_transport_t fake;
    memset(&fake, 0, sizeof(fake));
    rtcp_usbmux_transport_t transport = {
        .send = fake_send,
        .recv = fake_recv,
        .user = &fake,
    };
    rtcp_usbmux_init(&host, &transport);

    /* iOS appends four bytes to its VERSION reply.  The next frame starts at
     * offset 4, not at offset 0. */
    fake_packet_t *packet = &fake.received[fake.received_count++];
    size_t length = rtcp_usbmux_build_version(packet->data,
                                              sizeof(packet->data));
    memset(packet->data + length, 0, 4U);
    packet->length = length + 4U;
    assert(rtcp_usbmux_start(&host, 0U) == RTCP_OK);

    queue_device_frame(&fake, RTCP_USBMUX_LOCKDOWN_PORT, 1U, 100U, 1U,
                       RTCP_USBMUX_TH_SYN | RTCP_USBMUX_TH_ACK, NULL, 0U);
    rtcp_usbmux_connection_t *connection = NULL;
    assert(rtcp_usbmux_connect(&host, RTCP_USBMUX_LOCKDOWN_PORT, 0U, 500U,
                               &connection) == RTCP_OK);
    rtcp_usbmux_connection_close(&host, connection);
}

int main(void)
{
    test_connect_and_stream();
    test_connect_reset();
    test_connect_timeout();
    test_desynchronised_stream();
    test_two_streams();
    test_large_tls_record();
    test_back_to_back_frames();
    test_partial_frame();
    test_overflow_resets_and_reconnect();
    test_optional_reply_padding();
    test_version_negotiation();
    test_mux_sequence_echo();
    test_peer_header_fields();
    test_framed_exchange_and_stream_wait();
    test_control_diagnostic_bounds();
    puts("usbmux host test passed");
    return 0;
}
