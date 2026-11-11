/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtcp/usbmux.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

static void test_version_packet(void)
{
    static const uint8_t expected[20] = {
        0x00, 0x00, 0x00, 0x00, /* proto: version */
        0x00, 0x00, 0x00, 0x14, /* 20 bytes */
        0x00, 0x00, 0x00, 0x02, /* mux version 2, not the magic */
        0x00, 0x00, 0x00, 0x00, /* tx index */
        0x00, 0x00, 0x00, 0x00, /* reserved + zero payload */
    };
    uint8_t packet[32];
    memset(packet, 0xaa, sizeof(packet));
    assert(rtcp_usbmux_build_version(packet, sizeof(packet)) == 20U);
    assert(memcmp(packet, expected, sizeof(expected)) == 0);
    assert(rtcp_usbmux_build_version(packet, 19U) == 0U);
    assert(rtcp_usbmux_build_version(NULL, sizeof(packet)) == 0U);
}

static void test_setup_packet(void)
{
    static const uint8_t expected[17] = {
        0x00, 0x00, 0x00, 0x02, /* proto: setup */
        0x00, 0x00, 0x00, 0x11, /* 17 bytes */
        0xfe, 0xed, 0xfa, 0xce, /* magic */
        0x00, 0x00,             /* tx index 0 */
        0xff, 0xff,             /* initial rx sequence */
        0x07,                   /* flags */
    };
    uint8_t packet[32];
    assert(rtcp_usbmux_build_setup(packet, sizeof(packet), 0x07, 0U) == 17U);
    assert(memcmp(packet, expected, sizeof(expected)) == 0);

    /* The tx index is big-endian in the mux header. */
    assert(rtcp_usbmux_build_setup(packet, sizeof(packet), 0x07, 0x1234) ==
           17U);
    assert(packet[12] == 0x12U && packet[13] == 0x34U);
    assert(rtcp_usbmux_build_setup(packet, 16U, 0x07, 0U) == 0U);
}

static void test_tcp_packet(void)
{
    static const uint8_t expected[36] = {
        0x00, 0x00, 0x00, 0x06, /* proto: TCP */
        0x00, 0x00, 0x00, 0x24, /* 36 bytes */
        0xfe, 0xed, 0xfa, 0xce, /* magic */
        0x12, 0x34,             /* tx index */
        0x00, 0x00,             /* reserved */
        0x00, 0x01,             /* source port */
        0xf2, 0x7e,             /* destination port 62078 (lockdown) */
        0x11, 0x22, 0x33, 0x44, /* sequence */
        0x00, 0x00, 0x00, 0x00, /* acknowledgment */
        0x50,                   /* data offset 5 */
        0x02,                   /* SYN */
        0x02, 0x00,             /* window */
        0x00, 0x00, 0x00, 0x00, /* checksum + urgent */
    };
    uint8_t packet[64];
    assert(rtcp_usbmux_build_tcp(packet, sizeof(packet), 0x1234, 1U,
                                 RTCP_USBMUX_LOCKDOWN_PORT, 0x11223344U, 0U,
                                 RTCP_USBMUX_TH_SYN, NULL, 0U) == 36U);
    assert(memcmp(packet, expected, sizeof(expected)) == 0);

    static const uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef};
    assert(rtcp_usbmux_build_tcp(packet, sizeof(packet), 0U, 2U, 62078U, 1U,
                                 2U, RTCP_USBMUX_TH_ACK, payload,
                                 sizeof(payload)) == 40U);
    assert(packet[7] == 40U);
    assert(memcmp(packet + 36, payload, sizeof(payload)) == 0);
}

static void test_tcp_limits(void)
{
    uint8_t packet[64];
    static uint8_t payload[RTCP_USBMUX_MAX_PAYLOAD + 1U];
    assert(rtcp_usbmux_build_tcp(packet, sizeof(packet), 0U, 1U, 2U, 0U, 0U,
                                 RTCP_USBMUX_TH_SYN, payload,
                                 sizeof(payload)) == 0U);
    /* Payload fits the protocol but not the caller's buffer. */
    assert(rtcp_usbmux_build_tcp(packet, sizeof(packet), 0U, 1U, 2U, 0U, 0U,
                                 RTCP_USBMUX_TH_SYN, payload, 48U) == 0U);
    assert(rtcp_usbmux_build_tcp(packet, sizeof(packet), 0U, 1U, 2U, 0U, 0U,
                                 RTCP_USBMUX_TH_SYN, NULL, 1U) == 0U);
}

static void test_parse_frame(void)
{
    uint8_t stream[64];
    size_t length = rtcp_usbmux_build_setup(stream, sizeof(stream), 0x07, 1U);
    assert(length == 17U);
    /* Append a second frame to prove the parser only consumes the first. */
    length += rtcp_usbmux_build_version(stream + length,
                                        sizeof(stream) - length);

    size_t frame_length = 0U;
    size_t payload_length = 0U;
    uint32_t proto = 0U;
    const uint8_t *payload = NULL;
    assert(rtcp_usbmux_parse_frame(stream, length, &frame_length, &proto,
                                   &payload, &payload_length) == 1);
    assert(frame_length == 17U);
    assert(proto == RTCP_USBMUX_PROTO_SETUP);
    assert(payload_length == 1U && payload[0] == 0x07U);

    /* A truncated frame asks for more data instead of failing. */
    assert(rtcp_usbmux_parse_frame(stream, 16U, &frame_length, &proto, &payload,
                                   &payload_length) == 0);

    /* The version packet is accepted although it does not carry the magic. */
    assert(rtcp_usbmux_parse_frame(stream + 17U, length - 17U, &frame_length,
                                   &proto, &payload,
                                   &payload_length) == 1);
    assert(proto == RTCP_USBMUX_PROTO_VERSION);
    assert(frame_length == 20U && payload_length == 12U);
}

static void test_parse_frame_errors(void)
{
    uint8_t stream[36];
    size_t frame_length = 0U;
    size_t payload_length = 0U;
    uint32_t proto = 0U;
    const uint8_t *payload = NULL;

    /* Length below the minimum header size. */
    memset(stream, 0, sizeof(stream));
    stream[3] = 6U;
    assert(rtcp_usbmux_parse_frame(stream, sizeof(stream), &frame_length,
                                   &proto, &payload, &payload_length) ==
           RTCP_EIO);
    /* Absurd length, i.e. the receive stream is out of step. */
    stream[3] = 7U;
    stream[4] = 0x7fU;
    assert(rtcp_usbmux_parse_frame(stream, sizeof(stream), &frame_length,
                                   &proto, &payload, &payload_length) ==
           RTCP_EIO);
    /* TCP frame without the mux magic. */
    memset(stream, 0, sizeof(stream));
    stream[3] = 6U;
    stream[7] = 36U;
    assert(rtcp_usbmux_parse_frame(stream, sizeof(stream), &frame_length,
                                   &proto, &payload, &payload_length) ==
           RTCP_EIO);
}

static void test_parse_tcp(void)
{
    uint8_t packet[64];
    static const uint8_t data[] = {1, 2, 3, 4, 5};
    size_t length = rtcp_usbmux_build_tcp(packet, sizeof(packet), 0U, 0x1000U,
                                          62078U, 0x10203040U, 0x50607080U,
                                          RTCP_USBMUX_TH_ACK, data,
                                          sizeof(data));
    assert(length == 36U + sizeof(data));
    const uint8_t *payload = packet + RTCP_USBMUX_HEADER_SIZE;
    rtcp_usbmux_tcp_packet_t tcp;
    assert(rtcp_usbmux_parse_tcp(payload, length - RTCP_USBMUX_HEADER_SIZE,
                                 &tcp) == RTCP_OK);
    assert(tcp.source_port == 0x1000U);
    assert(tcp.destination_port == 62078U);
    assert(tcp.sequence == 0x10203040U);
    assert(tcp.acknowledgment == 0x50607080U);
    assert(tcp.flags == RTCP_USBMUX_TH_ACK);
    assert(tcp.window == RTCP_USBMUX_TCP_WINDOW);
    assert(tcp.payload_length == sizeof(data));
    assert(memcmp(tcp.payload, data, sizeof(data)) == 0);

    assert(rtcp_usbmux_parse_tcp(payload, 19U, &tcp) == RTCP_EINVAL);
}

int main(void)
{
    test_version_packet();
    test_setup_packet();
    test_tcp_packet();
    test_tcp_limits();
    test_parse_frame();
    test_parse_frame_errors();
    test_parse_tcp();
    puts("usbmux host test passed");
    return 0;
}
