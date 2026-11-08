/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bounded event-channel framing follows DiPlay ControlCipher.kt. The caller
 * owns connection serialization and TCP transport; this module only handles
 * authenticated frame boundaries and directional nonce counters.
 */
#include "airplay_control_cipher.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/chachapoly.h"
#include "mbedtls/platform_util.h"
#include "airplay_control_memory.h"

#define CIPHER_HEADER_BYTES 2U
#define CIPHER_TAG_BYTES 16U
#define CIPHER_RX_CAPACITY \
    (CIPHER_HEADER_BYTES + AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES + CIPHER_TAG_BYTES)

struct airplay_control_cipher {
    mbedtls_chachapoly_context read_ctx;
    mbedtls_chachapoly_context write_ctx;
    uint64_t read_counter;
    uint64_t write_counter;
    uint8_t pending[CIPHER_RX_CAPACITY];
    size_t pending_len;
    size_t expected_frame_len;
    bool failed;
};

static void make_nonce(uint64_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 4U);
    for (size_t i = 0; i < 8U; ++i) {
        nonce[4U + i] = (uint8_t)(counter >> (i * 8U));
    }
}

static airplay_control_cipher_result_t poison(
    airplay_control_cipher_t *cipher,
    airplay_control_cipher_result_t reason)
{
    if (cipher != NULL) {
        cipher->failed = true;
        mbedtls_platform_zeroize(cipher->pending, sizeof(cipher->pending));
        cipher->pending_len = 0U;
        cipher->expected_frame_len = 0U;
    }
    return reason;
}

airplay_control_cipher_result_t airplay_control_cipher_create(
    const uint8_t read_key[AIRPLAY_CONTROL_CIPHER_KEY_BYTES],
    const uint8_t write_key[AIRPLAY_CONTROL_CIPHER_KEY_BYTES],
    airplay_control_cipher_t **out_cipher)
{
    if (out_cipher == NULL) return AIRPLAY_CONTROL_CIPHER_INVALID_ARGUMENT;
    *out_cipher = NULL;
    if (read_key == NULL || write_key == NULL) {
        return AIRPLAY_CONTROL_CIPHER_INVALID_ARGUMENT;
    }

    airplay_control_cipher_t *cipher = calloc(1U, sizeof(*cipher));
    if (cipher == NULL) return AIRPLAY_CONTROL_CIPHER_CRYPTO_FAILED;
    mbedtls_chachapoly_init(&cipher->read_ctx);
    mbedtls_chachapoly_init(&cipher->write_ctx);
    int rc = mbedtls_chachapoly_setkey(&cipher->read_ctx, read_key);
    if (rc == 0) rc = mbedtls_chachapoly_setkey(&cipher->write_ctx, write_key);
    if (rc != 0) {
        airplay_control_cipher_destroy(cipher);
        return AIRPLAY_CONTROL_CIPHER_CRYPTO_FAILED;
    }
    *out_cipher = cipher;
    return AIRPLAY_CONTROL_CIPHER_OK;
}

airplay_control_cipher_result_t airplay_control_cipher_encrypt(
    airplay_control_cipher_t *cipher,
    const uint8_t *plaintext,
    size_t plaintext_len,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len)
{
    if (output_len != NULL) *output_len = 0U;
    if (cipher == NULL || output_len == NULL || output == NULL ||
        (plaintext == NULL && plaintext_len != 0U)) {
        return AIRPLAY_CONTROL_CIPHER_INVALID_ARGUMENT;
    }
    if (cipher->failed) return AIRPLAY_CONTROL_CIPHER_FAILED_STATE;
    if (plaintext_len > AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES) {
        return AIRPLAY_CONTROL_CIPHER_FRAME_TOO_LARGE;
    }
    if (output_capacity < plaintext_len + AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES) {
        return AIRPLAY_CONTROL_CIPHER_OUTPUT_TOO_SMALL;
    }

    output[0] = (uint8_t)(plaintext_len & 0xffU);
    output[1] = (uint8_t)((plaintext_len >> 8U) & 0xffU);
    uint8_t nonce[12];
    uint8_t empty_input = 0U;
    make_nonce(cipher->write_counter, nonce);
    int rc = mbedtls_chachapoly_encrypt_and_tag(
        &cipher->write_ctx, plaintext_len, nonce, output, CIPHER_HEADER_BYTES,
        plaintext_len == 0U ? &empty_input : plaintext,
        output + CIPHER_HEADER_BYTES,
        output + CIPHER_HEADER_BYTES + plaintext_len);
    mbedtls_platform_zeroize(nonce, sizeof(nonce));
    if (rc != 0) {
        return poison(cipher, AIRPLAY_CONTROL_CIPHER_CRYPTO_FAILED);
    }
    ++cipher->write_counter;
    *output_len = plaintext_len + AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES;
    return AIRPLAY_CONTROL_CIPHER_OK;
}

airplay_control_cipher_result_t airplay_control_cipher_decrypt(
    airplay_control_cipher_t *cipher,
    const uint8_t *input,
    size_t input_len,
    size_t *input_consumed,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len)
{
    if (input_consumed != NULL) *input_consumed = 0U;
    if (output_len != NULL) *output_len = 0U;
    if (cipher == NULL || input_consumed == NULL || output_len == NULL ||
        (input == NULL && input_len != 0U) ||
        (output == NULL && output_capacity != 0U)) {
        return AIRPLAY_CONTROL_CIPHER_INVALID_ARGUMENT;
    }
    if (cipher->failed) return AIRPLAY_CONTROL_CIPHER_FAILED_STATE;

    size_t consumed = 0U;
    size_t produced = 0U;
    for (;;) {
        if (cipher->expected_frame_len == 0U && cipher->pending_len >= CIPHER_HEADER_BYTES) {
            size_t plaintext_len = (size_t)cipher->pending[0] |
                                   ((size_t)cipher->pending[1] << 8U);
            if (plaintext_len > AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES) {
                *input_consumed = consumed;
                *output_len = produced;
                return poison(cipher, AIRPLAY_CONTROL_CIPHER_FRAME_TOO_LARGE);
            }
            cipher->expected_frame_len = CIPHER_HEADER_BYTES + plaintext_len + CIPHER_TAG_BYTES;
        }

        if (cipher->expected_frame_len != 0U &&
            cipher->pending_len == cipher->expected_frame_len) {
            size_t plaintext_len = cipher->expected_frame_len -
                                   CIPHER_HEADER_BYTES - CIPHER_TAG_BYTES;
            if (plaintext_len > output_capacity - produced) {
                *input_consumed = consumed;
                *output_len = produced;
                return AIRPLAY_CONTROL_CIPHER_OUTPUT_TOO_SMALL;
            }
            uint8_t nonce[12];
            uint8_t empty_output = 0U;
            make_nonce(cipher->read_counter, nonce);
            int rc = mbedtls_chachapoly_auth_decrypt(
                &cipher->read_ctx, plaintext_len, nonce, cipher->pending,
                CIPHER_HEADER_BYTES,
                cipher->pending + CIPHER_HEADER_BYTES + plaintext_len,
                cipher->pending + CIPHER_HEADER_BYTES,
                plaintext_len == 0U ? &empty_output : output + produced);
            mbedtls_platform_zeroize(nonce, sizeof(nonce));
            if (rc != 0) {
                *input_consumed = consumed;
                *output_len = produced;
                return poison(cipher, AIRPLAY_CONTROL_CIPHER_AUTH_FAILED);
            }
            ++cipher->read_counter;
            produced += plaintext_len;
            mbedtls_platform_zeroize(cipher->pending, cipher->pending_len);
            cipher->pending_len = 0U;
            cipher->expected_frame_len = 0U;
            continue;
        }

        if (consumed == input_len) break;
        size_t target = cipher->expected_frame_len;
        if (target == 0U) target = CIPHER_HEADER_BYTES;
        if (cipher->pending_len >= target || target > sizeof(cipher->pending)) {
            *input_consumed = consumed;
            *output_len = produced;
            return poison(cipher, AIRPLAY_CONTROL_CIPHER_FRAME_TOO_LARGE);
        }
        size_t take = target - cipher->pending_len;
        if (take > input_len - consumed) take = input_len - consumed;
        if (take > sizeof(cipher->pending) - cipher->pending_len) {
            *input_consumed = consumed;
            *output_len = produced;
            return poison(cipher, AIRPLAY_CONTROL_CIPHER_FRAME_TOO_LARGE);
        }
        memcpy(cipher->pending + cipher->pending_len, input + consumed, take);
        cipher->pending_len += take;
        consumed += take;
    }

    *input_consumed = consumed;
    *output_len = produced;
    return cipher->pending_len == 0U ? AIRPLAY_CONTROL_CIPHER_OK
                                     : AIRPLAY_CONTROL_CIPHER_INCOMPLETE;
}

void airplay_control_cipher_destroy(airplay_control_cipher_t *cipher)
{
    if (cipher == NULL) return;
    mbedtls_chachapoly_free(&cipher->read_ctx);
    mbedtls_chachapoly_free(&cipher->write_ctx);
    mbedtls_platform_zeroize(cipher, sizeof(*cipher));
    free(cipher);
}
