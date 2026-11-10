/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_audio.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/chachapoly.h"
#include "rtcp/rtcp.h"
#include "rtcp_linux.h"

#define WAIT_MS 500
#define TEST_KEY_BYTES AIRPLAY_AUDIO_KEY_BYTES

typedef struct {
    unsigned calls;
    uint16_t stream_type;
    uint8_t data[256];
    size_t len;
    airplay_audio_format_t format;
    uint32_t timestamp;
} pcm_capture_t;

static int capture_pcm(uint16_t stream_type, const uint8_t *data, size_t len,
                       const airplay_audio_format_t *format,
                       uint32_t rtp_timestamp, uint64_t arrival_us,
                       void *context)
{
    (void)arrival_us;
    pcm_capture_t *capture = context;
    ++capture->calls;
    capture->stream_type = stream_type;
    assert(len <= sizeof(capture->data));
    memcpy(capture->data, data, len);
    capture->len = len;
    capture->format = *format;
    capture->timestamp = rtp_timestamp;
    return RTCP_OK;
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

static int wait_readable(const struct rtcp_net *net, int fd)
{
    struct rtcp_pollfd pfd = {.fd = fd, .events = RTCP_POLLIN};
    int ready = net->poll(&pfd, 1, WAIT_MS);
    return ready > 0 && (pfd.revents & RTCP_POLLIN);
}

static void write_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)(value >> 16U);
    out[2] = (uint8_t)(value >> 8U);
    out[3] = (uint8_t)value;
}

/* Builds the wire datagram the phone sends: RTP header, AEAD ciphertext,
 * Poly1305 tag, then the eight-byte little-endian nonce tail. */
static size_t build_datagram(const uint8_t key[TEST_KEY_BYTES], uint16_t seq,
                             uint32_t timestamp, uint32_t ssrc,
                             const uint8_t *pcm, size_t pcm_len,
                             uint64_t nonce_counter, uint8_t *out,
                             size_t capacity)
{
    const size_t total = AIRPLAY_AUDIO_OVERHEAD_BYTES + pcm_len;
    if (capacity < total) return 0U;
    out[0] = 0x80U; /* RTP v2 */
    out[1] = 0x60U; /* payload type 96 */
    out[2] = (uint8_t)(seq >> 8U);
    out[3] = (uint8_t)seq;
    write_be32(out + 4U, timestamp);
    write_be32(out + 8U, ssrc);

    uint8_t nonce[12];
    memset(nonce, 0, 4U);
    for (size_t i = 0U; i < 8U; ++i) {
        nonce[4U + i] = (uint8_t)(nonce_counter >> (8U * i));
    }

    mbedtls_chachapoly_context cipher;
    mbedtls_chachapoly_init(&cipher);
    assert(mbedtls_chachapoly_setkey(&cipher, key) == 0);
    uint8_t tag[AIRPLAY_AUDIO_TAG_BYTES];
    assert(mbedtls_chachapoly_encrypt_and_tag(
               &cipher, pcm_len, nonce, out + 4U, 8U, pcm, out + 12U, tag) == 0);
    mbedtls_chachapoly_free(&cipher);
    memcpy(out + 12U + pcm_len, tag, sizeof(tag));
    memcpy(out + 12U + pcm_len + sizeof(tag), nonce + 4U, 8U);
    return total;
}

static void test_format_mapping(void)
{
    airplay_audio_format_t format;
    assert(airplay_audio_format_from_bits(0x800U, &format));
    assert(format.sample_rate == 44100U && format.channels == 2U);
    assert(airplay_audio_format_from_bits(0x400U, &format));
    assert(format.sample_rate == 44100U && format.channels == 1U);
    assert(airplay_audio_format_from_bits(0x4U, &format));
    assert(format.sample_rate == 8000U && format.channels == 1U);
    assert(airplay_audio_format_from_bits(0x8000U, &format));
    assert(format.sample_rate == 48000U && format.channels == 2U);
    assert(airplay_audio_format_from_bits(0x4000U, &format));
    assert(format.sample_rate == 48000U && format.channels == 1U);
    /* A composite capability mask falls back to 44.1 kHz stereo, matching
     * playport's AudioStreamCodec.fromFormatBits. */
    assert(airplay_audio_format_from_bits(0x3fcU | 0xc00U, &format));
    assert(format.sample_rate == 44100U && format.channels == 2U);
    /* Encoded formats and zero are refused. */
    assert(!airplay_audio_format_from_bits(0x400000U, &format));
    assert(!airplay_audio_format_from_bits(0x800000U, &format));
    assert(!airplay_audio_format_from_bits(0x70000000U, &format));
    assert(!airplay_audio_format_from_bits(0U, &format));
    assert(airplay_audio_stream_type_is_supported(100U));
    assert(airplay_audio_stream_type_is_supported(101U));
    assert(airplay_audio_stream_type_is_supported(102U));
    assert(!airplay_audio_stream_type_is_supported(110U));
}

static void test_start_validation(void)
{
    uint8_t key[TEST_KEY_BYTES];
    memset(key, 0x11, sizeof(key));
    pcm_capture_t capture = {0};
    airplay_audio_stream_config_t config = {
        .stream_type = AIRPLAY_AUDIO_STREAM_MAIN,
        .audio_format_bits = 0x800U,
        .on_pcm = capture_pcm,
        .context = &capture,
    };
    memcpy(config.output_key, key, sizeof(key));
    airplay_audio_stream_t *stream = NULL;
    config.stream_type = 110U; /* screen, not audio */
    assert(airplay_audio_stream_start(&rtcp_os_linux, &rtcp_net_linux, &config,
                                      &stream) == RTCP_EINVAL);
    assert(stream == NULL);
    config.stream_type = AIRPLAY_AUDIO_STREAM_MAIN;
    config.audio_format_bits = 0x400000U; /* AAC-LC is not decoded here. */
    assert(airplay_audio_stream_start(&rtcp_os_linux, &rtcp_net_linux, &config,
                                      &stream) == RTCP_ENOTSUP);
    assert(stream == NULL);
}

static void test_decrypts_and_rejects(void)
{
    uint8_t key[TEST_KEY_BYTES];
    for (size_t i = 0U; i < sizeof(key); ++i) key[i] = (uint8_t)(0x20U + i);
    pcm_capture_t capture = {0};
    airplay_audio_stream_config_t config = {
        .stream_type = AIRPLAY_AUDIO_STREAM_MAIN,
        .audio_format_bits = 0x800U,
        .on_pcm = capture_pcm,
        .context = &capture,
    };
    memcpy(config.output_key, key, sizeof(key));
    airplay_audio_stream_t *stream = NULL;
    assert(airplay_audio_stream_start(&rtcp_os_linux, &rtcp_net_linux, &config,
                                      &stream) == RTCP_OK);
    assert(stream != NULL);
    assert(airplay_audio_stream_get_data_port(stream) != 0U);
    assert(airplay_audio_stream_get_control_port(stream) != 0U);
    assert(airplay_audio_stream_get_stream_type(stream) ==
           AIRPLAY_AUDIO_STREAM_MAIN);
    const airplay_audio_format_t *format = airplay_audio_stream_get_format(stream);
    assert(format != NULL && format->sample_rate == 44100U &&
           format->channels == 2U);

    int client = rtcp_net_linux.udp_open(RTCP_AF_INET, 0U, false);
    assert(client >= 0);
    rtcp_addr_t data_addr = loopback(airplay_audio_stream_get_data_port(stream));

    static const uint8_t pcm[] = {
        0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00,
    };
    uint8_t wire[256];
    size_t wire_len = build_datagram(key, 1U, 1234U, 0x11223344U, pcm,
                                     sizeof(pcm), 7U, wire, sizeof(wire));
    assert(wire_len == AIRPLAY_AUDIO_OVERHEAD_BYTES + sizeof(pcm));
    assert(rtcp_net_linux.udp_sendto(client, &data_addr, wire, wire_len) ==
           (int)wire_len);
    assert(wait_readable(&rtcp_net_linux,
                         airplay_audio_stream_get_data_fd(stream)));
    airplay_audio_stream_poll(stream, true, false);
    assert(capture.calls == 1U);
    assert(capture.stream_type == AIRPLAY_AUDIO_STREAM_MAIN);
    assert(capture.len == sizeof(pcm));
    assert(memcmp(capture.data, pcm, sizeof(pcm)) == 0);
    assert(capture.timestamp == 1234U);
    assert(capture.format.sample_rate == 44100U && capture.format.channels == 2U);
    assert(airplay_audio_stream_datagrams(stream) == 1U);
    assert(airplay_audio_stream_auth_failures(stream) == 0U);

    /* Tampering must fail authentication and deliver nothing. */
    wire[12U] ^= 0x01U;
    assert(rtcp_net_linux.udp_sendto(client, &data_addr, wire, wire_len) ==
           (int)wire_len);
    assert(wait_readable(&rtcp_net_linux,
                         airplay_audio_stream_get_data_fd(stream)));
    airplay_audio_stream_poll(stream, true, false);
    assert(capture.calls == 1U);
    assert(airplay_audio_stream_auth_failures(stream) == 1U);

    /* Short datagrams are counted and dropped. */
    uint8_t short_wire[8] = {0x80U, 0x60U};
    assert(rtcp_net_linux.udp_sendto(client, &data_addr, short_wire,
                                     sizeof(short_wire)) ==
           (int)sizeof(short_wire));
    assert(wait_readable(&rtcp_net_linux,
                         airplay_audio_stream_get_data_fd(stream)));
    airplay_audio_stream_poll(stream, true, false);
    assert(capture.calls == 1U);
    assert(airplay_audio_stream_datagrams(stream) == 3U);

    /* The control port is bound and drained without touching the decoder. */
    rtcp_addr_t control_addr =
        loopback(airplay_audio_stream_get_control_port(stream));
    uint8_t control[16] = {0x80U, 0xc9U};
    assert(rtcp_net_linux.udp_sendto(client, &control_addr, control,
                                     sizeof(control)) == (int)sizeof(control));
    assert(wait_readable(&rtcp_net_linux,
                         airplay_audio_stream_get_control_fd(stream)));
    airplay_audio_stream_poll(stream, false, true);
    assert(capture.calls == 1U);
    assert(airplay_audio_stream_datagrams(stream) == 3U);

    airplay_audio_stream_stop(stream, &rtcp_net_linux);
    rtcp_net_linux.close(client);
}

static void test_alt_stream_type(void)
{
    uint8_t key[TEST_KEY_BYTES];
    memset(key, 0x5a, sizeof(key));
    pcm_capture_t capture = {0};
    airplay_audio_stream_config_t config = {
        .stream_type = AIRPLAY_AUDIO_STREAM_ALT,
        .audio_format_bits = 0x400U, /* 44.1 kHz mono */
        .on_pcm = capture_pcm,
        .context = &capture,
    };
    memcpy(config.output_key, key, sizeof(key));
    airplay_audio_stream_t *stream = NULL;
    assert(airplay_audio_stream_start(&rtcp_os_linux, &rtcp_net_linux, &config,
                                      &stream) == RTCP_OK);
    int client = rtcp_net_linux.udp_open(RTCP_AF_INET, 0U, false);
    assert(client >= 0);
    rtcp_addr_t data_addr = loopback(airplay_audio_stream_get_data_port(stream));
    static const uint8_t pcm[4] = {0xaa, 0xbb, 0xcc, 0xdd};
    uint8_t wire[128];
    size_t wire_len = build_datagram(key, 9U, 99U, 1U, pcm, sizeof(pcm), 0U,
                                     wire, sizeof(wire));
    assert(rtcp_net_linux.udp_sendto(client, &data_addr, wire, wire_len) ==
           (int)wire_len);
    assert(wait_readable(&rtcp_net_linux,
                         airplay_audio_stream_get_data_fd(stream)));
    airplay_audio_stream_poll(stream, true, false);
    assert(capture.calls == 1U);
    assert(capture.stream_type == AIRPLAY_AUDIO_STREAM_ALT);
    assert(capture.format.sample_rate == 44100U && capture.format.channels == 1U);
    airplay_audio_stream_stop(stream, &rtcp_net_linux);
    rtcp_net_linux.close(client);
}

int main(void)
{
    test_format_mapping();
    test_start_validation();
    test_decrypts_and_rejects();
    test_alt_stream_type();
    puts("airplay audio stream host test passed");
    return 0;
}
