#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "iap2_core.h"

#define CAPTURE_BYTES 4096

typedef struct {
    uint8_t tx[CAPTURE_BYTES];
    size_t tx_len;
    uint16_t last_message_id;
    uint16_t last_acked_message_id;
    size_t last_body_len;
    unsigned messages;
    unsigned acked_callbacks;
    unsigned acked_callbacks_when_success_message_received;
    iap2_core_state_t last_state;
} harness_t;

static bool capture_tx(const uint8_t *bytes, size_t len, void *context)
{
    harness_t *h = context;
    if (len > sizeof(h->tx) - h->tx_len) {
        return false;
    }
    memcpy(h->tx + h->tx_len, bytes, len);
    h->tx_len += len;
    return true;
}

static void capture_message(uint16_t id, const uint8_t *body, size_t len, void *context)
{
    (void)body;
    harness_t *h = context;
    h->last_message_id = id;
    h->last_body_len = len;
    ++h->messages;
    if (id == 0xaa05U) {
        h->acked_callbacks_when_success_message_received = h->acked_callbacks;
    }
}

static void capture_state(iap2_core_state_t state, const char *reason, void *context)
{
    (void)reason;
    ((harness_t *)context)->last_state = state;
}

static void capture_csm_acked(uint16_t message_id, void *context)
{
    harness_t *h = context;
    h->last_acked_message_id = message_id;
    ++h->acked_callbacks;
}

static uint8_t checksum(const uint8_t *bytes, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum = (uint8_t)(sum + bytes[i]);
    }
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

static void clear_tx(harness_t *h)
{
    h->tx_len = 0;
}

int main(void)
{
    harness_t h = {0};
    iap2_core_t *core = iap2_core_create(capture_tx, capture_message, capture_state, &h);
    assert(core != NULL);

    assert(iap2_core_start(core, 0));
    static const uint8_t marker[] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
    assert(h.tx_len == sizeof(marker));
    assert(memcmp(h.tx, marker, sizeof(marker)) == 0);
    clear_tx(&h);

    /* Fragment the peer marker as it can arrive over several RFCOMM callbacks. */
    iap2_core_feed(core, marker, 2, 1);
    iap2_core_feed(core, marker + 2, sizeof(marker) - 2, 2);
    assert(iap2_core_state(core) == IAP2_CORE_NEGOTIATING);
    assert(h.tx_len > 9 && h.tx[0] == 0xff && h.tx[1] == 0x5a);
    clear_tx(&h);

    uint8_t sync[19] = {
        1, 1, 0x08, 0x00, 0x0f, 0xa0, 0x01, 0xf4, 4, 1,
        10, 0, 1, 11, 2, 1, 12, 1, 2,
    };
    uint8_t frame[128];
    size_t frame_len = make_peer_frame(0x80, 30, 0, 0, sync, sizeof(sync), frame, sizeof(frame));
    for (size_t i = 0; i < frame_len; ++i) {
        iap2_core_feed(core, frame + i, 1, 3 + i);
    }
    assert(iap2_core_state(core) == IAP2_CORE_NEGOTIATING);
    assert(h.tx_len == 9); /* Immediate acknowledgement of peer synchronization. */
    clear_tx(&h);

    frame_len = make_peer_frame(0x40, 30, 99, 0, NULL, 0, frame, sizeof(frame));
    iap2_core_feed(core, frame, frame_len, 100);
    assert(iap2_core_state(core) == IAP2_CORE_READY);
    assert(h.last_state == IAP2_CORE_READY);
    clear_tx(&h);

    static const uint8_t start_identification[] = {0x40, 0x40, 0x00, 0x06, 0x1d, 0x00};
    frame_len = make_peer_frame(0x40, 31, 99, 10, start_identification,
                                sizeof(start_identification), frame, sizeof(frame));
    iap2_core_feed(core, frame, 4, 101);
    iap2_core_feed(core, frame + 4, frame_len - 4, 102);
    assert(h.messages == 1);
    assert(h.last_message_id == 0x1d00);
    assert(h.last_body_len == 0);
    assert(h.tx_len == 9); /* Data packet is acknowledged. */
    clear_tx(&h);

    static const uint8_t body[] = {0xaa, 0x55};
    assert(iap2_core_send_csm_with_ack_callback(core, 0x1d01, body, sizeof(body),
                                                capture_csm_acked, &h, 103));
    assert(h.acked_callbacks == 0); /* Enqueue/transmit is not a peer link ACK. */
    assert(h.tx_len == 9 + 6 + sizeof(body) + 1);
    assert(h.tx[4] == 0x40 && h.tx[5] == 100 && h.tx[7] == 10);
    assert(h.tx[9] == 0x40 && h.tx[10] == 0x40 && h.tx[13] == 0x1d);
    assert(iap2_core_rx_frames(core) == 1);

    size_t first_tx_len = h.tx_len;
    assert(iap2_core_send_csm_with_ack_callback(core, 0x1d02, body, sizeof(body),
                                                capture_csm_acked, &h, 104));
    assert(h.acked_callbacks == 0); /* The queued CSM has not reached the peer yet. */
    assert(h.tx_len == first_tx_len); /* One in-flight packet; the next CSM is bounded/queued. */
    frame_len = make_peer_frame(0x40, 31, 100, 0, NULL, 0, frame, sizeof(frame));
    iap2_core_feed(core, frame, frame_len, 105);
    assert(h.acked_callbacks == 1 && h.last_acked_message_id == 0x1d01);
    assert(h.tx_len == first_tx_len + first_tx_len);
    assert(h.tx[first_tx_len + 5] == 101);
    assert(h.tx[first_tx_len + 13] == 0x1d && h.tx[first_tx_len + 14] == 0x02);
    assert(h.acked_callbacks == 1); /* Sending the dequeued frame still needs its own ACK. */
    frame_len = make_peer_frame(0x40, 31, 101, 0, NULL, 0, frame, sizeof(frame));
    iap2_core_feed(core, frame, frame_len, 106);
    assert(h.acked_callbacks == 2 && h.last_acked_message_id == 0x1d02);

    /* An ACK and a peer success message may be piggybacked in one link frame.
       The response ACK callback must run before dispatching the payload. */
    assert(iap2_core_send_csm_with_ack_callback(core, 0x1d03, body, sizeof(body),
                                                capture_csm_acked, &h, 107));
    static const uint8_t auth_success[] = {0x40, 0x40, 0x00, 0x06, 0xaa, 0x05};
    frame_len = make_peer_frame(0x40, 32, 102, 10, auth_success,
                                sizeof(auth_success), frame, sizeof(frame));
    iap2_core_feed(core, frame, frame_len, 108);
    assert(h.acked_callbacks == 3 && h.last_acked_message_id == 0x1d03);
    assert(h.last_message_id == 0xaa05);
    assert(h.acked_callbacks_when_success_message_received == 3);

    /* Bad frame checksum is dropped without corrupting the established session. */
    clear_tx(&h);
    frame_len = make_peer_frame(0x40, 31, 101, 0, NULL, 0, frame, sizeof(frame));
    frame[8] ^= 0x01;
    iap2_core_feed(core, frame, frame_len, 104);
    assert(iap2_core_state(core) == IAP2_CORE_READY);
    assert(iap2_core_bad_frames(core) > 0);

    /* A repeated synchronization frame must not reset an established receive sequence. */
    frame_len = make_peer_frame(0x80, 200, 0, 0, sync, sizeof(sync), frame, sizeof(frame));
    iap2_core_feed(core, frame, frame_len, 105);
    assert(iap2_core_state(core) == IAP2_CORE_READY);

    assert(iap2_core_send_csm(core, 0x1d04, body, sizeof(body), 109));
    assert(h.acked_callbacks == 3);

    static const uint8_t gap_csm[] = {0x40, 0x40, 0x00, 0x06, 0x12, 0x34};
    /* ACK plus extended-ACK control payload is not a CSM data packet. */
    clear_tx(&h);
    frame_len = make_peer_frame(0x60, 33, 102, 10, gap_csm,
                                sizeof(gap_csm), frame, sizeof(frame));
    iap2_core_feed(core, frame, frame_len, 109);
    assert(h.messages == 2);
    assert(h.tx_len == 0);

    /* An out-of-order peer packet requests retransmission of the whole unbuffered gap. */
    frame_len = make_peer_frame(0x40, 34, 102, 10, gap_csm,
                                sizeof(gap_csm), frame, sizeof(frame));
    iap2_core_feed(core, frame, frame_len, 110);
    assert(h.messages == 2); /* Sequence 34 is not dispatched before sequence 33. */
    assert(h.tx_len == 12 && h.tx[4] == 0x20);
    assert(h.tx[6] == 32); /* Cumulative ACK remains at the last accepted sequence. */
    assert(h.tx[9] == 33 && h.tx[10] == 34);

    /* An extended ACK listing our outstanding packet causes immediate retransmission. */
    const uint8_t missing_local_packet[] = {103};
    frame_len = make_peer_frame(0x20, 34, 102, 0, missing_local_packet,
                                sizeof(missing_local_packet), frame, sizeof(frame));
    size_t extended_ack_len = h.tx_len;
    iap2_core_feed(core, frame, frame_len, 111);
    size_t retransmit_len = 9U + 6U + sizeof(body) + 1U;
    assert(h.tx_len == extended_ack_len + retransmit_len);
    assert(h.tx[extended_ack_len + 5] == 103);
    assert(h.tx[extended_ack_len + 13] == 0x1d && h.tx[extended_ack_len + 14] == 0x04);
    size_t tx_after_extended_ack = h.tx_len;
    iap2_core_advance_time(core, 112);
    assert(h.tx_len == tx_after_extended_ack); /* The extended ACK rearmed the timer. */

    /* Once the missing sequence arrives, the peer's later packet is accepted on retry. */
    static const uint8_t gap_csm_2[] = {0x40, 0x40, 0x00, 0x06, 0x12, 0x35};
    frame_len = make_peer_frame(0x40, 33, 103, 10, gap_csm_2,
                                sizeof(gap_csm_2), frame, sizeof(frame));
    iap2_core_feed(core, frame, frame_len, 113);
    assert(h.messages == 3 && h.last_message_id == 0x1235);
    frame_len = make_peer_frame(0x40, 34, 103, 10, gap_csm,
                                sizeof(gap_csm), frame, sizeof(frame));
    iap2_core_feed(core, frame, frame_len, 114);
    assert(h.messages == 4 && h.last_message_id == 0x1234);

    /* Receive sequence numbers wrap from 255 to 0 without creating a false gap. */
    clear_tx(&h);
    static const uint8_t ea_payload[] = {0x01};
    for (unsigned sequence = 35U; sequence <= UINT8_MAX; ++sequence) {
        frame_len = make_peer_frame(0x40, (uint8_t)sequence, 103, 11,
                                    ea_payload, sizeof(ea_payload), frame, sizeof(frame));
        iap2_core_feed(core, frame, frame_len, 200U + sequence);
    }
    assert(iap2_core_state(core) == IAP2_CORE_READY);

    /* A gap across the wrap point advertises missing sequence 0 and packet 1. */
    clear_tx(&h);
    frame_len = make_peer_frame(0x40, 1, 103, 10, gap_csm,
                                sizeof(gap_csm), frame, sizeof(frame));
    iap2_core_feed(core, frame, frame_len, 500);
    assert(h.messages == 4);
    assert(h.tx_len == 12 && h.tx[4] == 0x20 && h.tx[6] == 255);
    assert(h.tx[9] == 0 && h.tx[10] == 1);
    frame_len = make_peer_frame(0x40, 0, 103, 10, gap_csm_2,
                                sizeof(gap_csm_2), frame, sizeof(frame));
    iap2_core_feed(core, frame, frame_len, 501);
    assert(h.messages == 5 && h.last_message_id == 0x1235);
    frame_len = make_peer_frame(0x40, 1, 103, 10, gap_csm,
                                sizeof(gap_csm), frame, sizeof(frame));
    iap2_core_feed(core, frame, frame_len, 502);
    assert(h.messages == 6 && h.last_message_id == 0x1234);

    iap2_core_reset(core, "host_test_reset");
    assert(h.acked_callbacks == 3); /* Reset abandons the callback; it cannot authenticate later. */

    iap2_core_destroy(core);
    puts("iap2_core host test passed");
    return 0;
}
