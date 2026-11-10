/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtcp/usbmux.h"

#include <string.h>

#include "rtcp/rtcp.h"

#define USBMUX_MAX_FRAME_LENGTH (1U << 20)
#define USBMUX_TCP_FLAGS_OFFSET 13U

static void write_be16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8U);
    out[1] = (uint8_t)value;
}

static void write_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)(value >> 16U);
    out[2] = (uint8_t)(value >> 8U);
    out[3] = (uint8_t)value;
}

static uint16_t read_be16(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8U) | in[1]);
}

static uint32_t read_be32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24U) | ((uint32_t)in[1] << 16U) |
           ((uint32_t)in[2] << 8U) | in[3];
}

static void write_header(uint8_t *out, uint32_t proto, uint32_t frame_length,
                         uint32_t magic, uint16_t tx_index)
{
    write_be32(out, proto);
    write_be32(out + 4U, frame_length);
    write_be32(out + 8U, magic);
    write_be16(out + 12U, tx_index);
    write_be16(out + 14U, 0U);
}

size_t rtcp_usbmux_build_version(uint8_t *output, size_t capacity)
{
    if (output == NULL || capacity < RTCP_USBMUX_VERSION_PACKET_SIZE) {
        return 0U;
    }
    /* The version packet is special: the third word is the protocol
     * version, not 0xfeedface.  The remaining payload is zero. */
    write_header(output, RTCP_USBMUX_PROTO_VERSION,
                 RTCP_USBMUX_VERSION_PACKET_SIZE, RTCP_USBMUX_VERSION, 0U);
    memset(output + RTCP_USBMUX_HEADER_SIZE, 0,
           RTCP_USBMUX_VERSION_PACKET_SIZE - RTCP_USBMUX_HEADER_SIZE);
    return RTCP_USBMUX_VERSION_PACKET_SIZE;
}

size_t rtcp_usbmux_build_setup(uint8_t *output, size_t capacity, uint8_t flags,
                               uint16_t tx_index)
{
    size_t frame_length = RTCP_USBMUX_HEADER_SIZE + 1U;
    if (output == NULL || capacity < frame_length) {
        return 0U;
    }
    write_header(output, RTCP_USBMUX_PROTO_SETUP, (uint32_t)frame_length,
                 RTCP_USBMUX_MAGIC, tx_index);
    output[RTCP_USBMUX_HEADER_SIZE] = flags;
    return frame_length;
}

size_t rtcp_usbmux_build_tcp(uint8_t *output, size_t capacity, uint16_t tx_index,
                             uint16_t source_port, uint16_t destination_port,
                             uint32_t sequence, uint32_t acknowledgment,
                             uint8_t flags, const void *payload,
                             size_t payload_length)
{
    if (output == NULL ||
        payload_length > RTCP_USBMUX_MAX_PAYLOAD ||
        (payload == NULL && payload_length != 0U)) {
        return 0U;
    }
    size_t frame_length = RTCP_USBMUX_HEADER_SIZE +
                          RTCP_USBMUX_TCP_HEADER_SIZE + payload_length;
    if (frame_length > capacity) {
        return 0U;
    }
    write_header(output, RTCP_USBMUX_PROTO_TCP, (uint32_t)frame_length,
                 RTCP_USBMUX_MAGIC, tx_index);

    uint8_t *tcp = output + RTCP_USBMUX_HEADER_SIZE;
    write_be16(tcp, source_port);
    write_be16(tcp + 2U, destination_port);
    write_be32(tcp + 4U, sequence);
    write_be32(tcp + 8U, acknowledgment);
    tcp[12] = 0x50U; /* data offset 5 words */
    tcp[13] = flags;
    write_be16(tcp + 14U, RTCP_USBMUX_TCP_WINDOW);
    write_be32(tcp + 16U, 0U); /* checksum and urgent pointer */
    if (payload_length != 0U) {
        memcpy(tcp + RTCP_USBMUX_TCP_HEADER_SIZE, payload, payload_length);
    }
    return frame_length;
}

int rtcp_usbmux_parse_frame(const uint8_t *data, size_t length,
                            size_t *frame_length, uint32_t *proto,
                            const uint8_t **payload, size_t *payload_length)
{
    if (data == NULL || frame_length == NULL || proto == NULL ||
        payload == NULL || payload_length == NULL) {
        return RTCP_EINVAL;
    }
    if (length < 8U) {
        return 0;
    }

    uint32_t frame_proto = read_be32(data);
    uint32_t frame_size = read_be32(data + 4U);
    if (frame_size < 8U || frame_size > USBMUX_MAX_FRAME_LENGTH) {
        return RTCP_EIO;
    }
    if (length < frame_size) {
        return 0;
    }
    if (frame_size < RTCP_USBMUX_HEADER_SIZE) {
        /* The version packet is the only frame that may be shorter than the
         * full mux header (8 bytes proto+length is not valid on its own). */
        return RTCP_EIO;
    }
    if (frame_proto != RTCP_USBMUX_PROTO_VERSION &&
        read_be32(data + 8U) != RTCP_USBMUX_MAGIC) {
        return RTCP_EIO;
    }

    *frame_length = frame_size;
    *proto = frame_proto;
    *payload = data + RTCP_USBMUX_HEADER_SIZE;
    *payload_length = frame_size - RTCP_USBMUX_HEADER_SIZE;
    return 1;
}

int rtcp_usbmux_parse_tcp(const uint8_t *payload, size_t payload_length,
                          rtcp_usbmux_tcp_packet_t *packet)
{
    if (payload == NULL || packet == NULL ||
        payload_length < RTCP_USBMUX_TCP_HEADER_SIZE) {
        return RTCP_EINVAL;
    }
    packet->source_port = read_be16(payload);
    packet->destination_port = read_be16(payload + 2U);
    packet->sequence = read_be32(payload + 4U);
    packet->acknowledgment = read_be32(payload + 8U);
    packet->flags = payload[USBMUX_TCP_FLAGS_OFFSET];
    packet->window = read_be16(payload + 14U);
    packet->payload = payload + RTCP_USBMUX_TCP_HEADER_SIZE;
    packet->payload_length = payload_length - RTCP_USBMUX_TCP_HEADER_SIZE;
    return RTCP_OK;
}
