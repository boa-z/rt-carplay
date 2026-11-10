/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * USBMUX v2 host and its TCP-like connection state machine.
 *
 * The host owns the bulk pipe through a pair of transport callbacks, sends
 * the version/setup handshake, opens streams to the iPhone's services (the
 * first one is always lockdown, 62078) and multiplexes them over one pipe.
 *
 * The implementation is a pump, not a thread: the owner calls
 * rtcp_usbmux_pump() with the idle timeout it can afford, and the host reads
 * one bulk transfer, parses all complete frames in it and dispatches them.
 * This fits the existing single poll thread and keeps the state machine
 * testable with a fake transport.
 *
 * Sequence numbers follow the reference implementation: the receiver accepts
 * payload in arrival order, acknowledges it and increments its own sequence
 * by the payload length.  There is no retransmission or out-of-order queue;
 * a USB bulk pipe does not reorder data, and the reference does the same.
 */
#ifndef RTCP_USBMUX_HOST_H
#define RTCP_USBMUX_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtcp/usbmux.h"

#define RTCP_USBMUX_MAX_CONNECTIONS 2U
/* One bulk transfer may carry a maximum-size TLS record (or two ordinary
 * frames) before the owning thread gets to call connection_recv().  A
 * 4 KiB ring dropped the stream exactly when it carried a full record. */
#define RTCP_USBMUX_CONN_RX_CAPACITY RTCP_USBMUX_MAX_PAYLOAD
#define RTCP_USBMUX_REASSEMBLY_CAPACITY \
    (RTCP_USBMUX_MAX_PAYLOAD + RTCP_USBMUX_TCP_HEADER_SIZE + \
     RTCP_USBMUX_HEADER_SIZE)

/* Send one complete mux frame.  Returns 0 or a negative RTCP_* error. */
typedef int (*rtcp_usbmux_send_fn)(void *user, const uint8_t *data,
                                   size_t length);
/* Read up to capacity bytes from the bulk IN endpoint.  Returns the number of
 * bytes, RTCP_ETIMEDOUT when the timeout expired, or another negative
 * RTCP_* error.  A return of 0 means an empty bulk transfer. */
typedef int (*rtcp_usbmux_recv_fn)(void *user, uint8_t *data, size_t capacity,
                                   uint32_t timeout_ms);

typedef struct {
    bool used;
    bool syn_sent;
    bool established;
    bool reset;
    bool remote_closed;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t tx_sequence;
    uint32_t tx_acknowledgment;
    uint8_t rx[RTCP_USBMUX_CONN_RX_CAPACITY];
    size_t rx_head;
    size_t rx_tail;
    size_t rx_count;
} rtcp_usbmux_connection_t;

typedef struct {
    rtcp_usbmux_send_fn send;
    rtcp_usbmux_recv_fn recv;
    void *user;
} rtcp_usbmux_transport_t;

typedef struct {
    rtcp_usbmux_transport_t transport;
    uint16_t next_local_port;
    uint16_t tx_index;
    bool started;
    rtcp_usbmux_connection_t connections[RTCP_USBMUX_MAX_CONNECTIONS];
    uint8_t reassembly[RTCP_USBMUX_REASSEMBLY_CAPACITY];
    size_t reassembly_length;
    /* Whole frame scratch: a bulk OUT transfer must not split a frame, or a
     * packet-aligned write could append a zero-length packet in the middle
     * of it. */
    uint8_t tx_frame[RTCP_USBMUX_MAX_PAYLOAD + RTCP_USBMUX_TCP_HEADER_SIZE +
                     RTCP_USBMUX_HEADER_SIZE];
} rtcp_usbmux_host_t;

/* Reset the host state and bind the transport.  Does not start USB traffic. */
void rtcp_usbmux_init(rtcp_usbmux_host_t *host,
                      const rtcp_usbmux_transport_t *transport);

/* Send the 20-byte version packet, wait briefly for the device's answer (as
 * the reference does) and send the setup packet.  Returns RTCP_OK or the
 * transport error.  A timeout while waiting for the version answer is not an
 * error: some devices answer only after setup. */
int rtcp_usbmux_start(rtcp_usbmux_host_t *host, uint32_t now_ms);

/* Open a stream to remote_port: send SYN and pump until the device answers
 * with SYN|ACK or the timeout elapses.  Returns RTCP_OK and a connection
 * pointer, or RTCP_ETIMEDOUT / RTCP_EIO. */
int rtcp_usbmux_connect(rtcp_usbmux_host_t *host, uint16_t remote_port,
                        uint32_t now_ms, uint32_t timeout_ms,
                        rtcp_usbmux_connection_t **connection);

/* Read from the bulk pipe and dispatch every complete frame.  Returns the
 * number of frames handled, or a negative RTCP_* error when the stream is
 * desynchronised or the transport failed.  RTCP_ETIMEDOUT from the transport
 * is normal (nothing arrived) and returns 0. */
int rtcp_usbmux_pump(rtcp_usbmux_host_t *host, uint32_t timeout_ms,
                     uint32_t now_ms);

/* Send bytes on a stream, splitting them into mux-sized frames.  Returns
 * RTCP_OK or a negative error (RTCP_ESTATE when the stream is not
 * established or was closed). */
int rtcp_usbmux_connection_send(rtcp_usbmux_host_t *host,
                                rtcp_usbmux_connection_t *connection,
                                const void *data, size_t length);

/* Copy received payload out of a stream.  Returns the number of bytes copied
 * (> 0), 0 when no data is buffered, or RTCP_ESTATE when the stream closed
 * and its buffer is drained. */
int rtcp_usbmux_connection_recv(rtcp_usbmux_host_t *host,
                                rtcp_usbmux_connection_t *connection,
                                void *data, size_t capacity);

/* Send FIN and release the slot. */
void rtcp_usbmux_connection_close(rtcp_usbmux_host_t *host,
                                  rtcp_usbmux_connection_t *connection);

#endif /* RTCP_USBMUX_HOST_H */
