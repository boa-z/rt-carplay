/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implements the bounded screen DataStream AEAD path described in DiPlay's
 * ScreenStream.kt. See COPYING and THIRD_PARTY_NOTICES.md for notices.
 */
#include "airplay_screen.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/chachapoly.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"

#if !defined(MBEDTLS_CHACHAPOLY_C)
#error "AirPlay screen decryption requires MBEDTLS_CHACHAPOLY_C"
#endif
#if !defined(MBEDTLS_HKDF_C)
#error "AirPlay screen key derivation requires MBEDTLS_HKDF_C"
#endif

struct airplay_screen_stream {
    uint8_t key[AIRPLAY_SCREEN_KEY_BYTES];
    size_t max_body_bytes;
    uint64_t frame_counter;
    uint32_t auth_failures;
    uint8_t config[AIRPLAY_SCREEN_CONFIG_BYTES];
    size_t config_len;
    airplay_screen_message_cb_t callback;
    void *context;
    mbedtls_chachapoly_context cipher;
};

static void make_nonce(uint64_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 4);
    for (size_t i = 0; i < 8; ++i) {
        nonce[4U + i] = (uint8_t)(counter >> (8U * i));
    }
}

airplay_screen_stream_t *airplay_screen_stream_create(const uint8_t output_key[AIRPLAY_SCREEN_KEY_BYTES],
                                                      size_t max_body_bytes,
                                                      airplay_screen_message_cb_t callback,
                                                      void *context)
{
    if (output_key == NULL || callback == NULL || max_body_bytes < AIRPLAY_SCREEN_TAG_BYTES + 4U ||
        max_body_bytes > UINT32_MAX) {
        return NULL;
    }
    airplay_screen_stream_t *stream = calloc(1, sizeof(*stream));
    if (stream == NULL) return NULL;
    stream->max_body_bytes = max_body_bytes;
    stream->callback = callback;
    stream->context = context;
    mbedtls_chachapoly_init(&stream->cipher);
    if (mbedtls_chachapoly_setkey(&stream->cipher, output_key) != 0) {
        mbedtls_chachapoly_free(&stream->cipher);
        free(stream);
        return NULL;
    }
    memcpy(stream->key, output_key, sizeof(stream->key));
    return stream;
}

void airplay_screen_stream_destroy(airplay_screen_stream_t *stream)
{
    if (stream == NULL) return;
    mbedtls_chachapoly_free(&stream->cipher);
    memset(stream->key, 0, sizeof(stream->key));
    free(stream);
}

int airplay_screen_derive_output_key(const uint8_t *shared_secret,
                                           size_t shared_secret_len,
                                           uint64_t stream_connection_id,
                                           uint8_t output_key[AIRPLAY_SCREEN_KEY_BYTES])
{
    static const char salt_prefix[] = "DataStream-Salt";
    static const char info[] = "DataStream-Output-Encryption-Key";
    if (shared_secret == NULL || shared_secret_len == 0 || output_key == NULL) {
        return RTCP_EINVAL;
    }

    char salt[sizeof(salt_prefix) - 1U + 20U + 1U];
    int salt_len = snprintf(salt, sizeof(salt), "%s%" PRIu64,
                            salt_prefix, stream_connection_id);
    if (salt_len <= 0 || (size_t)salt_len >= sizeof(salt)) {
        memset(output_key, 0, AIRPLAY_SCREEN_KEY_BYTES);
        return RTCP_EINVAL;
    }
    const mbedtls_md_info_t *sha512 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    if (sha512 == NULL) {
        memset(salt, 0, sizeof(salt));
        memset(output_key, 0, AIRPLAY_SCREEN_KEY_BYTES);
        return RTCP_ENOTSUP;
    }
    int result = mbedtls_hkdf(sha512,
                              (const unsigned char *)salt, (size_t)salt_len,
                              shared_secret, shared_secret_len,
                              (const unsigned char *)info, sizeof(info) - 1U,
                              output_key, AIRPLAY_SCREEN_KEY_BYTES);
    memset(salt, 0, sizeof(salt));
    if (result != 0) {
        memset(output_key, 0, AIRPLAY_SCREEN_KEY_BYTES);
        return RTCP_EIO;
    }
    return RTCP_OK;
}

int airplay_screen_stream_process(airplay_screen_stream_t *stream,
                                       const uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES],
                                       uint8_t *body,
                                       size_t body_len,
                                       size_t body_prefix_capacity,
                                       uint64_t arrival_time_us)
{
    if (stream == NULL || header == NULL || (body == NULL && body_len != 0)) {
        return RTCP_EINVAL;
    }
    airplay_screen_header_info_t info;
    if (!airplay_screen_parse_header(header, body_len, &info) || body_len > stream->max_body_bytes) {
        return RTCP_EINVAL;
    }

    /* DiPlay ignores non-video messages after consuming their bounded body.
     * They must not consume an AEAD video nonce or reach the media callback. */
    if (info.opcode != 0U && info.opcode != 1U) return RTCP_OK;

    if (info.opcode == 1U) {
        size_t config_len = 0;
        if (!airplay_screen_extract_avcc_to_annex_b(body, body_len,
                                                   stream->config, sizeof(stream->config),
                                                   &config_len)) {
            return RTCP_ENOTSUP;
        }
        stream->config_len = config_len;
        return stream->callback(AIRPLAY_SCREEN_MESSAGE_CONFIG_H264,
                                stream->config, stream->config_len, 0U,
                                info.sender_time_ns, arrival_time_us, stream->context);
    }

    if (body_len < AIRPLAY_SCREEN_TAG_BYTES) return RTCP_EINVAL;
    size_t ciphertext_len = body_len - AIRPLAY_SCREEN_TAG_BYTES;
    uint8_t nonce[12];
    make_nonce(stream->frame_counter, nonce);
    const uint8_t *tag = body + ciphertext_len;
    int result = mbedtls_chachapoly_auth_decrypt(&stream->cipher, ciphertext_len,
                                                nonce, header, AIRPLAY_SCREEN_HEADER_BYTES,
                                                tag, body, body);
    if (result != 0) {
        ++stream->auth_failures;
        return RTCP_EIO;
    }
    ++stream->frame_counter;
    if (!airplay_screen_convert_length_prefixed_to_annex_b(body, ciphertext_len)) {
        return RTCP_EIO;
    }
    return stream->callback(AIRPLAY_SCREEN_MESSAGE_FRAME_H264,
                            body, ciphertext_len, body_prefix_capacity,
                            info.sender_time_ns, arrival_time_us, stream->context);
}

uint64_t airplay_screen_stream_frame_counter(const airplay_screen_stream_t *stream)
{
    return stream != NULL ? stream->frame_counter : 0;
}

uint32_t airplay_screen_stream_auth_failures(const airplay_screen_stream_t *stream)
{
    return stream != NULL ? stream->auth_failures : 0;
}

bool airplay_screen_crypto_self_test(void)
{
    static const uint8_t plaintext[] = {'c', 'a', 'r', 'p', 'l', 'a', 'y'};
    static const uint8_t aad[] = {'h', 'd', 'r'};
    static const uint8_t hkdf_expected[AIRPLAY_SCREEN_KEY_BYTES] = {
        0x7c, 0x1c, 0x9c, 0x6f, 0x96, 0xf4, 0xef, 0x45,
        0x46, 0x1a, 0xbb, 0xaa, 0x9c, 0xa5, 0xe2, 0xbd,
        0x72, 0x37, 0x4e, 0xad, 0x48, 0x6b, 0xd1, 0x87,
        0x0b, 0x0a, 0xd3, 0xab, 0x36, 0xae, 0x3b, 0x1a,
    };
    uint8_t hkdf_ikm[22];
    memset(hkdf_ikm, 0x0b, sizeof(hkdf_ikm));
    uint8_t derived[AIRPLAY_SCREEN_KEY_BYTES];
    uint8_t key[AIRPLAY_SCREEN_KEY_BYTES] = {0};
    uint8_t nonce[12] = {0};
    uint8_t ciphertext[sizeof(plaintext)];
    uint8_t tag[AIRPLAY_SCREEN_TAG_BYTES];
    uint8_t recovered[sizeof(plaintext)];
    bool ok = false;
    mbedtls_chachapoly_context cipher;
    mbedtls_chachapoly_init(&cipher);
    if (mbedtls_chachapoly_setkey(&cipher, key) == 0 &&
        mbedtls_chachapoly_encrypt_and_tag(&cipher, sizeof(plaintext), nonce,
                                           aad, sizeof(aad), plaintext,
                                           ciphertext, tag) == 0) {
        /* ESP-IDF 5.5.3's bundled Mbed TLS supports exact in-place XOR. */
        memcpy(recovered, ciphertext, sizeof(ciphertext));
        if (mbedtls_chachapoly_auth_decrypt(&cipher, sizeof(ciphertext), nonce,
                                            aad, sizeof(aad), tag,
                                            recovered, recovered) == 0 &&
            memcmp(plaintext, recovered, sizeof(plaintext)) == 0 &&
            airplay_screen_derive_output_key(hkdf_ikm, sizeof(hkdf_ikm),
                                             7, derived) == RTCP_OK &&
            memcmp(derived, hkdf_expected, sizeof(hkdf_expected)) == 0) {
            ok = true;
        }
    }
    mbedtls_chachapoly_free(&cipher);
    memset(key, 0, sizeof(key));
    memset(recovered, 0, sizeof(recovered));
    memset(hkdf_ikm, 0, sizeof(hkdf_ikm));
    memset(derived, 0, sizeof(derived));
    return ok;
}
