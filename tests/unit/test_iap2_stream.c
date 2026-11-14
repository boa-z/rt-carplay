/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iap2_stream.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

typedef struct {
    rtcp_stream_t stream;
    const uint8_t *data;
    size_t data_len;
    size_t data_pos;
    int empty_result;
    size_t reads;
    uint8_t tx[2048];
    size_t tx_len;
} fake_stream_t;

static int fake_read(void *context, void *buffer, size_t length,
                     uint32_t timeout_ms)
{
    (void)timeout_ms;
    fake_stream_t *fake = context;
    ++fake->reads;
    if (fake->data_pos >= fake->data_len) {
        return fake->empty_result;
    }
    size_t available = fake->data_len - fake->data_pos;
    size_t count = available < length ? available : length;
    memcpy(buffer, fake->data + fake->data_pos, count);
    fake->data_pos += count;
    return (int)count;
}

static int fake_write(void *context, const void *data, size_t length)
{
    fake_stream_t *fake = context;
    if (length > sizeof(fake->tx) - fake->tx_len) return RTCP_ENOMEM;
    memcpy(fake->tx + fake->tx_len, data, length);
    fake->tx_len += length;
    return RTCP_OK;
}

static bool tx_to_stream(const uint8_t *bytes, size_t len, void *context)
{
    fake_stream_t *fake = context;
    return fake->stream.ops->write(fake->stream.context, bytes, len) == RTCP_OK;
}

static uint8_t checksum(const uint8_t *bytes, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum = (uint8_t)(sum + bytes[i]);
    return (uint8_t)(0U - sum);
}

static size_t make_peer_frame(uint8_t control, uint8_t seq, uint8_t ack,
                              uint8_t sid, const uint8_t *payload,
                              size_t payload_len, uint8_t *out,
                              size_t capacity)
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

static bool tx_has_message(const fake_stream_t *fake, uint16_t message_id)
{
    /* CSM header: 0x40 0x40, length, message id. */
    for (size_t i = 0; i + 6U <= fake->tx_len; ++i) {
        if (fake->tx[i] == 0x40U && fake->tx[i + 1U] == 0x40U &&
            fake->tx[i + 4U] == (uint8_t)(message_id >> 8) &&
            fake->tx[i + 5U] == (uint8_t)message_id) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    fake_stream_t fake;
    memset(&fake, 0, sizeof(fake));
    fake.stream.ops = &(rtcp_stream_ops_t){.read = fake_read, .write = fake_write};
    fake.stream.context = &fake;
    fake.empty_result = RTCP_ETIMEDOUT;

    /* Peer marker, synchronisation, marker acknowledgement and then
     * StartIdentification; the link only accepts CSM after this handshake. */
    uint8_t input[160];
    size_t input_len = 0U;
    static const uint8_t peer_marker[6] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
    memcpy(input, peer_marker, sizeof(peer_marker));
    input_len += sizeof(peer_marker);
    const uint8_t sync[19] = {
        1, 1, 0x08, 0x00, 0x0f, 0xa0, 0x01, 0xf4, 4, 1,
        10, 0, 1, 11, 2, 1, 12, 1, 2,
    };
    input_len += make_peer_frame(0x80, 30, 0, 0, sync, sizeof(sync),
                                 input + input_len, sizeof(input) - input_len);
    input_len += make_peer_frame(0x40, 30, 99, 0, NULL, 0,
                                 input + input_len, sizeof(input) - input_len);
    const uint8_t csm[6] = {0x40, 0x40, 0x00, 0x06, 0x1d, 0x00};
    input_len += make_peer_frame(0x40, 31, 0, 10, csm, sizeof(csm),
                                 input + input_len, sizeof(input) - input_len);
    fake.data = input;
    fake.data_len = input_len;

    iap2_session_config_t config = {
        .identity = {
            .name = "rt-carplay stream",
            .model_identifier = "RTCP-TEST",
            .manufacturer = "rt-carplay",
            .serial_number = "001122334455",
            .firmware_version = "0.1",
            .hardware_version = "host",
            .language = "en",
            .wifi_ssid = "rtcp-test",
        },
        .tx = tx_to_stream,
        .context = &fake,
    };
    iap2_session_t *session = iap2_session_create(&config);
    assert(session != NULL);
    iap2_session_link_up(session, 0U);

    iap2_stream_rx_t rx;
    iap2_stream_rx_init(&rx, &fake.stream);
    assert(rx.stream.ops == fake.stream.ops);

    /* One read feeds both frames; the session answers identification. */
    assert(iap2_stream_rx_step(&rx, session, 100U, 10U) == RTCP_OK);
    assert(fake.reads == 1U);
    assert(tx_has_message(&fake, 0x1d01U));

    /* Idle timeout keeps the link usable. */
    assert(iap2_stream_rx_step(&rx, session, 100U, 20U) == RTCP_ETIMEDOUT);

    /* Peer close and transport errors are reported unchanged. */
    fake.empty_result = 0;
    assert(iap2_stream_rx_step(&rx, session, 100U, 30U) == 0);
    fake.empty_result = RTCP_EIO;
    assert(iap2_stream_rx_step(&rx, session, 100U, 40U) == RTCP_EIO);

    assert(iap2_stream_rx_step(NULL, session, 100U, 50U) == RTCP_EINVAL);
    assert(iap2_stream_rx_step(&rx, NULL, 100U, 50U) == RTCP_EINVAL);
    rx.stream.ops = NULL;
    assert(iap2_stream_rx_step(&rx, session, 100U, 50U) == RTCP_EINVAL);

    iap2_session_link_down(session);
    iap2_session_destroy(session);
    puts("iap2 stream host test passed");
    return 0;
}
