/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_screen_server.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/chachapoly.h"
#include "rtcp/rtcp.h"
#include "rtcp_linux.h"

#define WAIT_MS 500
#define MAX_BODY_BYTES (64U * 1024U)

typedef struct {
    unsigned calls;
    airplay_screen_message_type_t type;
    uint8_t bytes[32];
    size_t len;
    uint64_t sender_time_ns;
} capture_t;

static unsigned exit_count;
static uint64_t fake_now_us = 10000U;
static uint64_t sender_ntp = (UINT64_C(2) << 32) | UINT64_C(0x80000000);
static uint64_t fake_clock_us(void) { return fake_now_us; }
static bool stepped_clock;
static int measured_recv(int fd, void *buffer, size_t bytes)
{
    int n = rtcp_net_linux.recv(fd, buffer, bytes);
    if (n > 0 && stepped_clock) fake_now_us += 1000U;
    return n;
}

static int capture_message(airplay_screen_message_type_t type,
                           uint8_t *annex_b,
                           size_t annex_b_len,
                           size_t prefix_capacity,
                           uint64_t sender_time_ns,
                           uint64_t arrival_time_us,
                           void *context)
{
    capture_t *capture = context;
    assert(annex_b != NULL);
    assert(annex_b_len <= sizeof(capture->bytes));
    assert(prefix_capacity >= AIRPLAY_SCREEN_CONFIG_BYTES);
    (void)arrival_time_us;
    ++capture->calls;
    capture->type = type;
    capture->len = annex_b_len;
    capture->sender_time_ns = sender_time_ns;
    memcpy(capture->bytes, annex_b, annex_b_len);
    return RTCP_OK;
}

static void on_unexpected_exit(void *context)
{
    (void)context;
    ++exit_count;
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *out, uint64_t value)
{
    for (size_t i = 0U; i < 8U; ++i) out[i] = (uint8_t)(value >> (8U * i));
}

static void make_nonce(uint64_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 4U);
    for (size_t i = 0U; i < 8U; ++i) nonce[4U + i] = (uint8_t)(counter >> (8U * i));
}

static size_t seal_frame(const uint8_t key[AIRPLAY_SCREEN_KEY_BYTES],
                         uint64_t counter,
                         const uint8_t *plaintext,
                         size_t plaintext_len,
                         uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES],
                         uint8_t *body)
{
    size_t body_len = plaintext_len + AIRPLAY_SCREEN_TAG_BYTES;
    memset(header, 0, AIRPLAY_SCREEN_HEADER_BYTES);
    put_le32(header, (uint32_t)body_len);
    header[4] = 0U;
    put_le64(header + 8U, sender_ntp);
    uint8_t nonce[12];
    make_nonce(counter, nonce);
    mbedtls_chachapoly_context cipher;
    mbedtls_chachapoly_init(&cipher);
    assert(mbedtls_chachapoly_setkey(&cipher, key) == 0);
    assert(mbedtls_chachapoly_encrypt_and_tag(
               &cipher, plaintext_len, nonce, header,
               AIRPLAY_SCREEN_HEADER_BYTES, plaintext, body,
               body + plaintext_len) == 0);
    mbedtls_chachapoly_free(&cipher);
    return body_len;
}

static rtcp_addr_t loopback(uint16_t port)
{
    rtcp_addr_t address = {0};
    address.family = RTCP_AF_INET;
    address.addr[0] = 127U;
    address.addr[3] = 1U;
    address.port = port;
    return address;
}

static bool wait_event(int fd)
{
    struct rtcp_pollfd pfd = {.fd = fd, .events = RTCP_POLLIN};
    int ready = rtcp_net_linux.poll(&pfd, 1, WAIT_MS);
    return ready > 0 && pfd.revents != 0;
}

static bool send_all(int fd, const uint8_t *bytes, size_t len)
{
    size_t offset = 0U;
    while (offset < len) {
        int sent = rtcp_net_linux.send(fd, bytes + offset, len - offset);
        if (sent <= 0) return false;
        offset += (size_t)sent;
    }
    return true;
}

static void send_frame(int fd, const uint8_t *header, const uint8_t *body,
                       size_t body_len)
{
    assert(send_all(fd, header, AIRPLAY_SCREEN_HEADER_BYTES));
    assert(send_all(fd, body, body_len));
}

int main(void)
{
    static const uint8_t key[AIRPLAY_SCREEN_KEY_BYTES] = {
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
        0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
        0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
        0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    };
    static const uint8_t plaintext[] = {0, 0, 0, 2, 0x65, 0xaa};
    static const uint8_t expected_annex_b[] = {0, 0, 0, 1, 0x65, 0xaa};

    capture_t capture = {0};
    airplay_screen_server_config_t config = {0};
    config.requested_port = 0U;
    config.max_body_bytes = MAX_BODY_BYTES;
    config.on_message = capture_message;
    config.message_context = &capture;
    config.on_unexpected_exit = on_unexpected_exit;

    struct rtcp_os os = rtcp_os_linux;
    os.now_us = fake_clock_us;
    struct rtcp_net measured_net = rtcp_net_linux;
    measured_net.recv = measured_recv;
    airplay_screen_server_t *server = NULL;
    assert(airplay_screen_server_start(&os, &measured_net, key,
                                       &config, &server) == RTCP_OK);
    uint16_t port = airplay_screen_server_get_port(server);
    assert(port != 0U);

    rtcp_addr_t address = loopback(port);
    int client = rtcp_net_linux.connect(&address);
    assert(client >= 0);

    assert(wait_event(airplay_screen_server_get_fd(server)));
    airplay_screen_server_poll(server); /* accepts and returns */
    assert(airplay_screen_server_get_fd(server) != -1);

    uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES];
    uint8_t body[256];
    size_t body_len = seal_frame(key, 0U, plaintext, sizeof(plaintext), header,
                                 body);
    send_frame(client, header, body, body_len);

    for (unsigned i = 0U; i < 16U && capture.calls == 0U; ++i) {
        assert(wait_event(airplay_screen_server_get_fd(server)));
        airplay_screen_server_poll(server);
    }
    assert(capture.calls == 1U);
    assert(capture.type == AIRPLAY_SCREEN_MESSAGE_FRAME_H264);
    assert(capture.len == sizeof(expected_annex_b));
    assert(memcmp(capture.bytes, expected_annex_b, sizeof(expected_annex_b)) ==
           0);
    assert(capture.sender_time_ns == UINT64_C(2500000000));

    /* Arrival gaps and sender timestamps must be measured independently.
     * The last two-byte/body handoff also exercises assembly-time accounting. */
    const uint64_t arrivals[] = {45000U, 125000U, 375000U};
    for (unsigned sample = 0; sample < 3; ++sample) {
        sender_ntp += (UINT64_C(1) << 32) / 30U;
        body_len = seal_frame(key, sample + 1U, plaintext, sizeof(plaintext), header, body);
        fake_now_us = arrivals[sample] - 2000U;
        assert(send_all(client, header, sizeof(header)));
        assert(wait_event(airplay_screen_server_get_fd(server)));
        airplay_screen_server_poll(server); /* header, no body yet */
        assert(capture.calls == sample + 1U);
        fake_now_us = arrivals[sample];
        assert(send_all(client, body, body_len));
        for (unsigned retry = 0; retry < 16U && capture.calls < sample + 2U; ++retry) {
            assert(wait_event(airplay_screen_server_get_fd(server)));
            airplay_screen_server_poll(server);
        }
        assert(capture.calls == sample + 2U);
    }
    airplay_screen_server_stats_t stats;
    airplay_screen_server_get_stats(server, &stats);
    assert(stats.frames == 4U && stats.messages == 4U);
    assert(stats.frame_gap_count == 3U && stats.frame_gap_us == 365000U);
    assert(stats.frame_gap_max_us == 250000U);
    assert(stats.frame_gap_le40ms == 1U && stats.frame_gap_le100ms == 1U && stats.frame_gap_gt100ms == 1U);
    assert(stats.sender_gap_count == 3U && stats.sender_gap_max_us == 33333U);
    assert(stats.assembly_us == 6000U && stats.assembly_max_us == 2000U);

    /* One queued burst retains nonce/order and yields after at most 16
     * reads, rather than monopolizing the control/HID owner until empty. */
    unsigned baseline_batches = stats.read_batches;
    unsigned baseline_reads = stats.recv_calls;
    uint8_t burst[20 * (AIRPLAY_SCREEN_HEADER_BYTES + sizeof(plaintext) + AIRPLAY_SCREEN_TAG_BYTES)];
    size_t used = 0;
    for (unsigned i = 0; i < 20; ++i) {
        body_len = seal_frame(key, 4U + i, plaintext, sizeof(plaintext), header, body);
        memcpy(burst + used, header, sizeof(header)); used += sizeof(header);
        memcpy(burst + used, body, body_len); used += body_len;
    }
    assert(send_all(client, burst, used));
    assert(wait_event(airplay_screen_server_get_fd(server)));
    airplay_screen_server_poll(server);
    assert(capture.calls > 5U && capture.calls <= 12U);
    airplay_screen_server_get_stats(server, &stats);
    assert(stats.read_budget_yields >= 1U);
    unsigned before = capture.calls;
    stepped_clock = true;
    assert(wait_event(airplay_screen_server_get_fd(server)));
    airplay_screen_server_poll(server);
    assert(capture.calls - before <= 1U); /* 2 ms cooperative budget */
    stepped_clock = false;
    while (capture.calls < 24U) {
        assert(wait_event(airplay_screen_server_get_fd(server)));
        airplay_screen_server_poll(server);
    }
    assert(exit_count == 0U);
    airplay_screen_server_get_stats(server, &stats);
    assert(stats.frames == 24U && stats.read_batches < stats.recv_calls);
    printf("screen burst: 20 authenticated frames, reads=%u owner_dispatches=%u (old one-read path needs at least 40)\n",
           stats.recv_calls - baseline_reads, stats.read_batches - baseline_batches);

    /* Partial body unavailable for 1.25 s while the owner runs every 50 ms:
     * do not confuse missing input with the owner failing to run. */
    body_len = seal_frame(key, 24U, plaintext, sizeof(plaintext), header, body);
    airplay_screen_server_owner_tick(server);
    assert(send_all(client, header, sizeof(header)));
    assert(wait_event(airplay_screen_server_get_fd(server)));
    airplay_screen_server_poll(server);
    for (unsigned i = 0; i < 25; ++i) {
        fake_now_us += 50000U;
        airplay_screen_server_owner_tick(server);
    }
    airplay_screen_server_get_stats(server, &stats);
    assert(stats.pending_body_bytes == body_len && stats.pending_age_ms == 1250);
    assert(stats.owner_gap_max_us == 50000U && !stats.owner_gap_over100ms);
    assert(send_all(client, body, body_len));
    assert(wait_event(airplay_screen_server_get_fd(server)));
    airplay_screen_server_poll(server);
    airplay_screen_server_get_stats(server, &stats);
    assert(stats.partial_gap_max_us == 1250000U && stats.partial_gap_over100ms);
    assert(stats.recv_block_max_us <= 1000U && !stats.pending_body_bytes);
    assert(capture.calls == 25U);

    /* A frame that fails authentication kills the one-peer data stream. */
    body_len = seal_frame(key, 25U, plaintext, sizeof(plaintext), header, body);
    body[body_len - 1U] ^= 0x80U;
    send_frame(client, header, body, body_len);

    for (unsigned i = 0U; i < 16U && exit_count == 0U; ++i) {
        int fd = airplay_screen_server_get_fd(server);
        if (fd < 0) break;
        if (!wait_event(fd)) break;
        airplay_screen_server_poll(server);
    }
    assert(exit_count == 1U);
    assert(airplay_screen_server_get_fd(server) == -1);
    rtcp_net_linux.close(client);
    airplay_screen_server_stop(server, &rtcp_net_linux);
    assert(exit_count == 1U);

    /* Stopping a healthy server must not report an unexpected exit. */
    exit_count = 0U;
    capture.calls = 0U;
    server = NULL;
    assert(airplay_screen_server_start(&rtcp_os_linux, &rtcp_net_linux, key,
                                       &config, &server) == RTCP_OK);
    address = loopback(airplay_screen_server_get_port(server));
    client = rtcp_net_linux.connect(&address);
    assert(client >= 0);
    assert(wait_event(airplay_screen_server_get_fd(server)));
    airplay_screen_server_poll(server);
    airplay_screen_server_stop(server, &rtcp_net_linux);
    rtcp_net_linux.close(client);
    assert(exit_count == 0U);

    /* Argument checks. */
    airplay_screen_server_t *rejected = NULL;
    assert(airplay_screen_server_start(NULL, &rtcp_net_linux, key, &config,
                                       &rejected) == RTCP_EINVAL);
    assert(airplay_screen_server_start(&rtcp_os_linux, &rtcp_net_linux, NULL,
                                       &config, &rejected) == RTCP_EINVAL);
    assert(airplay_screen_server_start(&rtcp_os_linux, &rtcp_net_linux, key,
                                       NULL, &rejected) == RTCP_EINVAL);
    assert(airplay_screen_server_start(&rtcp_os_linux, &rtcp_net_linux, key,
                                       &config, NULL) == RTCP_EINVAL);
    config.max_body_bytes = 0U;
    assert(airplay_screen_server_start(&rtcp_os_linux, &rtcp_net_linux, key,
                                       &config, &rejected) == RTCP_EINVAL);
    assert(airplay_screen_server_get_port(NULL) == 0U);
    assert(airplay_screen_server_get_fd(NULL) == -1);
    airplay_screen_server_poll(NULL);
    airplay_screen_server_stop(NULL, &rtcp_net_linux);

    puts("airplay screen server host test passed");
    return 0;
}
