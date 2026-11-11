/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implements the bounded CarPlay audio DataStream receiver described in the
 * GPL-3.0 playport AudioStream.kt.  See NOTICE for the attribution.
 */
#include "airplay_audio.h"

#include <stdlib.h>
#include <string.h>

#include "mbedtls/chachapoly.h"
#include "rtcp/log.h"
#include "rtcp/rtcp.h"

#if !defined(MBEDTLS_CHACHAPOLY_C)
#error "AirPlay audio decryption requires MBEDTLS_CHACHAPOLY_C"
#endif

#define TAG "airplay_audio"

struct airplay_audio_stream {
    const struct rtcp_os *os;
    const struct rtcp_net *net;
    uint16_t stream_type;
    uint64_t format_bits;
    airplay_audio_format_t format;
    size_t max_datagram_bytes;
    airplay_audio_pcm_cb_t on_pcm;
    void *context;

    mbedtls_chachapoly_context cipher;
    bool cipher_ready;

    uint8_t *wire;
    int data_fd;
    int control_fd;
    uint16_t data_port;
    uint16_t control_port;

    uint32_t datagrams;
    uint32_t decrypted;
    uint32_t auth_failures;
    uint32_t short_packets;
    bool first_packet_logged;
    bool nonce_seen;
    uint64_t highest_nonce;
    uint64_t nonce_window;
};

typedef struct {
    uint64_t bits;
    uint32_t sample_rate;
    uint8_t channels;
} pcm_format_entry_t;

static const pcm_format_entry_t k_pcm_formats[] = {
    {0x4ULL, 8000U, 1U},
    {0x8ULL, 8000U, 2U},
    {0x10ULL, 16000U, 1U},
    {0x20ULL, 16000U, 2U},
    {0x40ULL, 24000U, 1U},
    {0x80ULL, 24000U, 2U},
    {0x100ULL, 32000U, 1U},
    {0x200ULL, 32000U, 2U},
    {0x400ULL, 44100U, 1U},
    {0x800ULL, 44100U, 2U},
    {0x4000ULL, 48000U, 1U},
    {0x8000ULL, 48000U, 2U},
};

/* AAC-LC 44.1/48 kHz stereo and every Opus bit: advertised by other
 * receivers but not decodable here, so they must never be mistaken for PCM. */
#define AUDIO_FORMAT_AAC 0xc00000ULL
#define AUDIO_FORMAT_OPUS 0x70000000ULL

bool airplay_audio_format_from_bits(uint64_t bits, airplay_audio_format_t *format)
{
    if (format == NULL || bits == 0U) return false;
    if ((bits & (AUDIO_FORMAT_AAC | AUDIO_FORMAT_OPUS)) != 0U) return false;
    for (size_t i = 0U; i < sizeof(k_pcm_formats) / sizeof(k_pcm_formats[0]); ++i) {
        if (bits == k_pcm_formats[i].bits) {
            format->sample_rate = k_pcm_formats[i].sample_rate;
            format->channels = k_pcm_formats[i].channels;
            return true;
        }
    }
    /* SETUP selects one format, unlike the /info capability mask. */
    return false;
}

bool airplay_audio_stream_type_is_supported(uint64_t stream_type)
{
    return stream_type == AIRPLAY_AUDIO_STREAM_MAIN ||
           stream_type == AIRPLAY_AUDIO_STREAM_ALT ||
           stream_type == AIRPLAY_AUDIO_STREAM_HIGH;
}

static void audio_stream_free(airplay_audio_stream_t *stream,
                              const struct rtcp_net *net)
{
    if (stream == NULL) return;
    if (stream->data_fd >= 0 && net != NULL && net->close != NULL) {
        net->close(stream->data_fd);
    }
    if (stream->control_fd >= 0 && net != NULL && net->close != NULL) {
        net->close(stream->control_fd);
    }
    if (stream->cipher_ready) {
        mbedtls_chachapoly_free(&stream->cipher);
        stream->cipher_ready = false;
    }
    free(stream->wire);
    memset(stream, 0, sizeof(*stream));
    free(stream);
}

int airplay_audio_stream_start(const struct rtcp_os *os,
                               const struct rtcp_net *net,
                               const airplay_audio_stream_config_t *config,
                               airplay_audio_stream_t **out_stream)
{
    if (out_stream == NULL) return RTCP_EINVAL;
    *out_stream = NULL;
    if (os == NULL || net == NULL || net->udp_open == NULL ||
        net->udp_recvfrom == NULL || config == NULL ||
        config->on_pcm == NULL ||
        !airplay_audio_stream_type_is_supported(config->stream_type)) {
        return RTCP_EINVAL;
    }
    uint8_t family = config->family == 0U ? RTCP_AF_INET : config->family;
    if (family != RTCP_AF_INET && family != RTCP_AF_INET6) return RTCP_EINVAL;

    airplay_audio_stream_t *stream = calloc(1U, sizeof(*stream));
    if (stream == NULL) return RTCP_ENOMEM;
    /* Set before any validation path can close partially built sockets. */
    stream->data_fd = -1;
    stream->control_fd = -1;
    stream->os = os;
    stream->net = net;
    stream->stream_type = config->stream_type;
    stream->format_bits = config->audio_format_bits;
    if (!airplay_audio_format_from_bits(config->audio_format_bits,
                                        &stream->format)) {
        audio_stream_free(stream, net);
        return RTCP_ENOTSUP;
    }
    stream->max_datagram_bytes = config->max_datagram_bytes != 0U
                                     ? config->max_datagram_bytes
                                     : AIRPLAY_AUDIO_DEFAULT_MAX_DATAGRAM_BYTES;
    if (stream->max_datagram_bytes <= AIRPLAY_AUDIO_OVERHEAD_BYTES) {
        audio_stream_free(stream, net);
        return RTCP_EINVAL;
    }
    stream->on_pcm = config->on_pcm;
    stream->context = config->context;
    stream->data_fd = -1;
    stream->control_fd = -1;

    stream->wire = calloc(1U, stream->max_datagram_bytes);
    if (stream->wire == NULL) {
        audio_stream_free(stream, net);
        return RTCP_ENOMEM;
    }
    mbedtls_chachapoly_init(&stream->cipher);
    stream->cipher_ready = true;
    if (mbedtls_chachapoly_setkey(&stream->cipher, config->output_key) != 0) {
        audio_stream_free(stream, net);
        return RTCP_EIO;
    }

    stream->data_fd = net->udp_open(family, 0U, false);
    if (stream->data_fd < 0) {
        RTCP_LOGE(TAG, "data_udp_open failed rc=%d", stream->data_fd);
        audio_stream_free(stream, net);
        return RTCP_EIO;
    }
    stream->control_fd = net->udp_open(family, 0U, false);
    if (stream->control_fd < 0) {
        RTCP_LOGE(TAG, "control_udp_open failed rc=%d", stream->control_fd);
        audio_stream_free(stream, net);
        return RTCP_EIO;
    }
    stream->data_port = net->local_port != NULL ? net->local_port(stream->data_fd)
                                                : 0U;
    stream->control_port = net->local_port != NULL
                               ? net->local_port(stream->control_fd)
                               : 0U;
    if (stream->data_port == 0U || stream->control_port == 0U) {
        audio_stream_free(stream, net);
        return RTCP_EIO;
    }

    *out_stream = stream;
    RTCP_LOGI(TAG, "stream=ready type=%u data_port=%u control_port=%u "
                   "rate=%u channels=%u format=0x%llx",
              (unsigned)stream->stream_type, (unsigned)stream->data_port,
              (unsigned)stream->control_port, (unsigned)stream->format.sample_rate,
              (unsigned)stream->format.channels,
              (unsigned long long)stream->format_bits);
    return RTCP_OK;
}

uint16_t airplay_audio_stream_get_data_port(const airplay_audio_stream_t *stream)
{
    return stream == NULL ? 0U : stream->data_port;
}

uint16_t airplay_audio_stream_get_control_port(const airplay_audio_stream_t *stream)
{
    return stream == NULL ? 0U : stream->control_port;
}

int airplay_audio_stream_get_data_fd(const airplay_audio_stream_t *stream)
{
    return stream == NULL ? -1 : stream->data_fd;
}

int airplay_audio_stream_get_control_fd(const airplay_audio_stream_t *stream)
{
    return stream == NULL ? -1 : stream->control_fd;
}

uint16_t airplay_audio_stream_get_stream_type(const airplay_audio_stream_t *stream)
{
    return stream == NULL ? 0U : stream->stream_type;
}

const airplay_audio_format_t *airplay_audio_stream_get_format(
    const airplay_audio_stream_t *stream)
{
    return stream == NULL ? NULL : &stream->format;
}

uint64_t airplay_audio_stream_get_format_bits(const airplay_audio_stream_t *stream)
{
    return stream == NULL ? 0U : stream->format_bits;
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | (uint16_t)bytes[1]);
}

static int process_buffer(airplay_audio_stream_t *stream, uint8_t *wire,
                          size_t len, uint64_t arrival_us)
{
    ++stream->datagrams;
    if (len < AIRPLAY_AUDIO_OVERHEAD_BYTES) {
        ++stream->short_packets;
        if (stream->short_packets == 1U) {
            RTCP_LOGW(TAG, "stream=%u short_datagram bytes=%u",
                      (unsigned)stream->stream_type, (unsigned)len);
        }
        return RTCP_EINVAL;
    }
    if (wire[0] != 0x80U) return RTCP_EINVAL; /* fixed RTP v2, no extensions/CSRC/padding */
    if (len > stream->max_datagram_bytes) {
        return RTCP_EINVAL;
    }

    /* Wire: header | ciphertext | 16-byte tag | 8-byte nonce tail. */
    const size_t nonce_offset = len - AIRPLAY_AUDIO_NONCE_BYTES;
    const size_t tag_offset = nonce_offset - AIRPLAY_AUDIO_TAG_BYTES;
    const size_t cipher_len = tag_offset - AIRPLAY_AUDIO_RTP_HEADER_BYTES;
    const uint8_t *tag = wire + tag_offset;
    uint8_t nonce[12];
    memset(nonce, 0, 4U);
    memcpy(nonce + 4U, wire + nonce_offset, AIRPLAY_AUDIO_NONCE_BYTES);

    uint8_t *payload = wire + AIRPLAY_AUDIO_RTP_HEADER_BYTES;
    int result = mbedtls_chachapoly_auth_decrypt(
        &stream->cipher, cipher_len, nonce, wire + 4U, 8U, tag, payload, payload);
    if (result != 0) {
        ++stream->auth_failures;
        if (stream->auth_failures == 1U) {
            RTCP_LOGW(TAG, "stream=%u decrypt_failed bytes=%u seq=%u ssrc=0x%08x",
                      (unsigned)stream->stream_type, (unsigned)len,
                      (unsigned)read_be16(wire + 2U), (unsigned)read_be32(wire + 8U));
        }
        return RTCP_EIO;
    }

    /* Commit the replay window only after successful authentication. */
    uint64_t counter = 0;
    for (unsigned i = 0; i < 8; ++i) counter |= (uint64_t)nonce[4 + i] << (8 * i);
    if (!stream->nonce_seen) {
        stream->nonce_seen = true;
        stream->highest_nonce = counter;
        stream->nonce_window = 1;
    } else if (counter > stream->highest_nonce) {
        uint64_t delta = counter - stream->highest_nonce;
        stream->nonce_window = delta >= 64 ? 1 : (stream->nonce_window << delta) | 1;
        stream->highest_nonce = counter;
    } else {
        uint64_t delta = stream->highest_nonce - counter;
        if (delta >= 64 || (stream->nonce_window & (UINT64_C(1) << delta))) return RTCP_EINVAL;
        stream->nonce_window |= UINT64_C(1) << delta;
    }
    if (cipher_len % (2U * stream->format.channels) != 0U) return RTCP_EINVAL;
    /* RTP L16 is big endian. The sink contract is little endian. */
    for (size_t i = 0; i < cipher_len; i += 2) {
        uint8_t byte = payload[i]; payload[i] = payload[i + 1]; payload[i + 1] = byte;
    }
    ++stream->decrypted;
    const uint32_t timestamp = read_be32(wire + 4U);
    if (!stream->first_packet_logged) {
        stream->first_packet_logged = true;
        RTCP_LOGI(TAG, "stream=%u first_packet pcm_bytes=%u seq=%u timestamp=%u",
                  (unsigned)stream->stream_type, (unsigned)cipher_len,
                  (unsigned)read_be16(wire + 2U), (unsigned)timestamp);
    }
    return stream->on_pcm(stream->stream_type, payload, cipher_len,
                          &stream->format, timestamp, arrival_us,
                          stream->context);
}

int airplay_audio_stream_process_datagram(airplay_audio_stream_t *stream,
                                          const uint8_t *datagram,
                                          size_t len,
                                          uint64_t arrival_us)
{
    if (stream == NULL || (datagram == NULL && len != 0U)) {
        return RTCP_EINVAL;
    }
    if (len > stream->max_datagram_bytes) return RTCP_EINVAL;
    if (len != 0U) memcpy(stream->wire, datagram, len);
    return process_buffer(stream, stream->wire, len, arrival_us);
}

void airplay_audio_stream_poll(airplay_audio_stream_t *stream,
                               bool data_readable,
                               bool control_readable)
{
    if (stream == NULL) return;
    if (data_readable && stream->data_fd >= 0) {
        rtcp_addr_t source;
        memset(&source, 0, sizeof(source));
        int received = stream->net->udp_recvfrom(stream->data_fd, stream->wire,
                                                 stream->max_datagram_bytes,
                                                 &source);
        if (received > 0) {
            uint64_t now_us = stream->os->now_us != NULL
                                  ? stream->os->now_us()
                                  : stream->os->now_ms() * 1000U;
            process_buffer(stream, stream->wire, (size_t)received, now_us);
        }
    }
    if (control_readable && stream->control_fd >= 0) {
        uint8_t control[128];
        rtcp_addr_t source;
        memset(&source, 0, sizeof(source));
        (void)stream->net->udp_recvfrom(stream->control_fd, control,
                                        sizeof(control), &source);
    }
}

uint32_t airplay_audio_stream_datagrams(const airplay_audio_stream_t *stream)
{
    return stream == NULL ? 0U : stream->datagrams;
}

uint32_t airplay_audio_stream_auth_failures(const airplay_audio_stream_t *stream)
{
    return stream == NULL ? 0U : stream->auth_failures;
}

void airplay_audio_stream_stop(airplay_audio_stream_t *stream,
                               const struct rtcp_net *net)
{
    audio_stream_free(stream, net);
}
