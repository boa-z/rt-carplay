/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usbmux_host.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"
#include "rtcp/usbmux.h"

#define FAKE_SENT_MAX 16U
#define FAKE_RX_MAX 8U

typedef struct {
    uint8_t data[4096];
    size_t length;
} fake_packet_t;

typedef struct {
    fake_packet_t sent[FAKE_SENT_MAX];
    unsigned sent_count;
    fake_packet_t received[FAKE_RX_MAX];
    unsigned received_count;
    unsigned received_read;
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
    (void)timeout_ms;
    fake_transport_t *fake = user;
    if (fake->received_read >= fake->received_count) {
        return RTCP_ETIMEDOUT;
    }
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
    assert(rtcp_usbmux_start(host, 0U) == RTCP_OK);
    /* Version packet, then setup. */
    assert(fake->sent_count == 2U);
    assert(fake->sent[0].length == RTCP_USBMUX_VERSION_PACKET_SIZE);
    assert(fake->sent[0].data[8] == 0U && fake->sent[0].data[11] == 2U);
    assert(fake->sent[1].length == 17U);
    assert(fake->sent[1].data[16] == 0x07U);
}

static void test_connect_and_stream(void)
{
    static rtcp_usbmux_host_t host;
    fake_transport_t fake;
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
    fake_transport_t fake;
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
    fake_transport_t fake;
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
    fake_transport_t fake;
    start_host(&host, &fake);

    fake_packet_t *packet = &fake.received[fake.received_count++];
    memset(packet->data, 0, sizeof(packet->data));
    packet->data[3] = RTCP_USBMUX_PROTO_TCP;
    packet->data[7] = 36U;
    packet->length = 36U;
    assert(rtcp_usbmux_pump(&host, 0U, 0U) == RTCP_EIO);
}

static void test_two_streams(void)
{
    static rtcp_usbmux_host_t host;
    fake_transport_t fake;
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

int main(void)
{
    test_connect_and_stream();
    test_connect_reset();
    test_connect_timeout();
    test_desynchronised_stream();
    test_two_streams();
    puts("usbmux host test passed");
    return 0;
}
