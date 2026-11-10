/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Apple USBMUX v2 wire format (the multiplexing layer used on the iPhone's
 * CarPlay USB configuration, and by usbmuxd on the normal configuration).
 *
 * USBMUX is not iAP2 and not NCM.  It is a small framing protocol that
 * carries several TCP-like streams over one bulk pipe:
 *
 *   frame  = proto:u32be | length:u32be | magic:u32be | tx:u16be | 0:u16
 *            | payload
 *
 * The first frame sent by the host is the 20-byte version packet; it is the
 * one packet whose third word is the mux version (2), not the magic.  The
 * host then sends a setup packet (proto 2, payload 0x07) and may open TCP
 * streams (proto 6) whose payload starts with a 20-byte TCP header in the
 * same layout Apple's usbmuxd uses.
 *
 * This header only defines the byte-level format.  The connection state
 * machine lives in core/usb/usbmux_host.c so the format can be unit tested
 * with fixed byte vectors.
 */
#ifndef RTCP_USBMUX_H
#define RTCP_USBMUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTCP_USBMUX_PROTO_VERSION 0U
#define RTCP_USBMUX_PROTO_SETUP 2U
#define RTCP_USBMUX_PROTO_TCP 6U

#define RTCP_USBMUX_MAGIC 0xfeedfaceU
#define RTCP_USBMUX_VERSION 2U
#define RTCP_USBMUX_HEADER_SIZE 16U
#define RTCP_USBMUX_VERSION_PACKET_SIZE 20U
#define RTCP_USBMUX_TCP_HEADER_SIZE 20U
/* Largest TCP payload in one mux frame.  A TLS record can be 16 KiB of
 * plaintext plus up to 2 KiB of expansion; 32 KiB also leaves room for a
 * second frame from the same bulk transfer.  The old 16 KiB limit rejected
 * a whole-record frame at the wire boundary. */
#define RTCP_USBMUX_MAX_PAYLOAD 32768U

/* The iPhone's lockdown/usbmuxd control port.  It is a TCP destination port
 * inside a USBMUX stream, not a UDP/TCP port on an IP network. */
#define RTCP_USBMUX_LOCKDOWN_PORT 62078U

#define RTCP_USBMUX_TH_FIN 0x01U
#define RTCP_USBMUX_TH_SYN 0x02U
#define RTCP_USBMUX_TH_RST 0x04U
#define RTCP_USBMUX_TH_ACK 0x10U

/* Apple's receivers advertise this window (LIVI uses 131072 >> 8). */
#define RTCP_USBMUX_TCP_WINDOW 0x0200U

typedef struct {
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t sequence;
    uint32_t acknowledgment;
    uint8_t flags;
    uint16_t window;
    const uint8_t *payload;
    size_t payload_length;
} rtcp_usbmux_tcp_packet_t;

/* Build the 20-byte version announcement.  Returns the length or 0 when the
 * output buffer is too small. */
size_t rtcp_usbmux_build_version(uint8_t *output, size_t capacity);

/* Build a setup packet (proto 2).  flags is 0x07 on the wire after the
 * version handshake. */
size_t rtcp_usbmux_build_setup(uint8_t *output, size_t capacity, uint8_t flags,
                               uint16_t tx_index);

/* Build a TCP stream packet (proto 6).  tx_index is the per-host frame
 * counter carried in the mux header.  Returns the length or 0 when the
 * payload is too large for the frame or the buffer is too small. */
size_t rtcp_usbmux_build_tcp(uint8_t *output, size_t capacity, uint16_t tx_index,
                             uint16_t source_port, uint16_t destination_port,
                             uint32_t sequence, uint32_t acknowledgment,
                             uint8_t flags, const void *payload,
                             size_t payload_length);

/*
 * Split one frame from a receive buffer.  On success returns 1 and sets
 * *frame_length, *proto and the payload pointer/length.  Returns 0 when the
 * buffer does not yet hold a complete frame (wait for more bulk data) and
 * RTCP_EIO when the header is not a valid USBMUX frame.  A frame length of
 * less than 8 or more than 1 MiB is treated as a desynchronised stream.
 */
int rtcp_usbmux_parse_frame(const uint8_t *data, size_t length,
                            size_t *frame_length, uint32_t *proto,
                            const uint8_t **payload, size_t *payload_length);

/* Parse the 20-byte TCP header of a proto 6 payload.  Returns RTCP_OK or
 * RTCP_EINVAL when the frame is too short. */
int rtcp_usbmux_parse_tcp(const uint8_t *payload, size_t payload_length,
                          rtcp_usbmux_tcp_packet_t *packet);

#endif /* RTCP_USBMUX_H */
