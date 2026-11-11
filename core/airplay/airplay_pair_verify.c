/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pair-Verify message flow follows DiPlay PairVerify.kt and AirPlayCrypto.kt.
 * See THIRD_PARTY_NOTICES.md. Ed25519 operations and pairing lookup are
 * delegated to the configured platform provider.
 */
#include "airplay_pair_verify.h"

#include <stdlib.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "airplay_tlv8.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

#define PV_IDENTIFIER 0x01U
#define PV_PUBLIC_KEY 0x03U
#define PV_ENCRYPTED_DATA 0x05U
#define PV_STATE 0x06U
#define PV_ERROR 0x07U
#define PV_SIGNATURE 0x0aU
#define PV_ERROR_AUTHENTICATION 2U
#define PV_NONCE_BYTES 12U
#define PV_TAG_BYTES 16U
#define PV_SUBMESSAGE_MAX_BYTES 256U
#define PV_IDENTIFIER_STORAGE_BYTES AIRPLAY_PAIR_VERIFY_MAX_IDENTIFIER_BYTES

typedef enum {
    PV_SESSION_WAIT_M1 = 0,
    PV_SESSION_WAIT_M3,
    PV_SESSION_COMPLETE,
    PV_SESSION_FAILED,
} pv_session_state_t;

struct airplay_pair_verify {
    airplay_pair_verify_config_t provider;
    uint8_t accessory_identifier[PV_IDENTIFIER_STORAGE_BYTES];
    mbedtls_ecp_group group;
    mbedtls_mpi private_key;
    uint8_t accessory_ephemeral_public[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
    uint8_t controller_ephemeral_public[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
    uint8_t shared_secret[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
    uint8_t encryption_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
    airplay_pair_verify_control_keys_t control_keys;
    airplay_pair_verify_event_keys_t event_keys;
    pv_session_state_t state;
    bool random_failed;
};

static airplay_pair_verify_t *pair_verify_calloc(void)
{
    return calloc(1U, sizeof(airplay_pair_verify_t));
}

static void pair_verify_free(airplay_pair_verify_t *session)
{
    free(session);
}

static int pv_rng(void *context, unsigned char *output, size_t output_len)
{
    airplay_pair_verify_t *session = context;
    if (session == NULL || output == NULL ||
        !session->provider.random(output, output_len,
                                  session->provider.provider_context)) {
        if (session != NULL) session->random_failed = true;
        return -1;
    }
    return 0;
}

static void scrub(void *buffer, size_t length)
{
    if (buffer != NULL && length != 0U) {
        mbedtls_platform_zeroize(buffer, length);
    }
}

static airplay_pair_verify_result_t encode_error(uint8_t state,
                                                  uint8_t *response,
                                                  size_t response_capacity,
                                                  size_t *response_len)
{
    const uint8_t error = PV_ERROR_AUTHENTICATION;
    const airplay_tlv8_item_t items[] = {
        {.type = PV_STATE, .value = &state, .value_len = 1U},
        {.type = PV_ERROR, .value = &error, .value_len = 1U},
    };
    return airplay_tlv8_encode(items, 2U, response, response_capacity,
                               response_len) == AIRPLAY_TLV8_OK
               ? AIRPLAY_PAIR_VERIFY_AUTH_FAILED
               : AIRPLAY_PAIR_VERIFY_BUFFER_TOO_SMALL;
}

static airplay_pair_verify_result_t encode_response(
    const airplay_tlv8_item_t *items,
    size_t item_count,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    airplay_tlv8_result_t result = airplay_tlv8_encode(
        items, item_count, response, response_capacity, response_len);
    if (result == AIRPLAY_TLV8_OK) return AIRPLAY_PAIR_VERIFY_OK;
    return result == AIRPLAY_TLV8_TOO_LARGE
               ? AIRPLAY_PAIR_VERIFY_BUFFER_TOO_SMALL
               : AIRPLAY_PAIR_VERIFY_INVALID_ARGUMENT;
}

static airplay_pair_verify_result_t fail_session(
    airplay_pair_verify_t *session,
    uint8_t state,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len,
    airplay_pair_verify_result_t reason)
{
    session->state = PV_SESSION_FAILED;
    scrub(session->shared_secret, sizeof(session->shared_secret));
    scrub(session->encryption_key, sizeof(session->encryption_key));
    scrub(&session->control_keys, sizeof(session->control_keys));
    scrub(&session->event_keys, sizeof(session->event_keys));
    mbedtls_mpi_free(&session->private_key);
    mbedtls_mpi_init(&session->private_key);
    scrub(session->accessory_ephemeral_public,
          sizeof(session->accessory_ephemeral_public));
    scrub(session->controller_ephemeral_public,
          sizeof(session->controller_ephemeral_public));
    return encode_error(state, response, response_capacity, response_len) ==
                   AIRPLAY_PAIR_VERIFY_AUTH_FAILED
               ? reason
               : AIRPLAY_PAIR_VERIFY_BUFFER_TOO_SMALL;
}

static int derive_key(const uint8_t *input,
                      size_t input_len,
                      const char *salt,
                      const char *info,
                      uint8_t output[AIRPLAY_PAIR_VERIFY_KEY_BYTES])
{
    const mbedtls_md_info_t *sha512 =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    if (sha512 == NULL) return -1;
    return mbedtls_hkdf(sha512,
                        (const uint8_t *)salt, strlen(salt),
                        input, input_len,
                        (const uint8_t *)info, strlen(info),
                        output, AIRPLAY_PAIR_VERIFY_KEY_BYTES);
}

static void make_nonce(const char label[8], uint8_t nonce[PV_NONCE_BYTES])
{
    memset(nonce, 0, PV_NONCE_BYTES);
    memcpy(nonce + 4U, label, 8U);
}

static int chacha_encrypt(const uint8_t key[AIRPLAY_PAIR_VERIFY_KEY_BYTES],
                          const uint8_t nonce[PV_NONCE_BYTES],
                          const uint8_t *plaintext,
                          size_t plaintext_len,
                          uint8_t *output,
                          size_t output_capacity,
                          size_t *output_len)
{
    if (plaintext_len > output_capacity ||
        PV_TAG_BYTES > output_capacity - plaintext_len) {
        return -1;
    }
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    int rc = mbedtls_chachapoly_setkey(&ctx, key);
    if (rc == 0) {
        rc = mbedtls_chachapoly_encrypt_and_tag(&ctx, plaintext_len,
                                                nonce, NULL, 0U, plaintext,
                                                output,
                                                output + plaintext_len);
    }
    mbedtls_chachapoly_free(&ctx);
    if (rc == 0) *output_len = plaintext_len + PV_TAG_BYTES;
    return rc;
}

static int chacha_decrypt(const uint8_t key[AIRPLAY_PAIR_VERIFY_KEY_BYTES],
                          const uint8_t nonce[PV_NONCE_BYTES],
                          const uint8_t *input,
                          size_t input_len,
                          uint8_t *plaintext,
                          size_t plaintext_capacity,
                          size_t *plaintext_len)
{
    if (input_len < PV_TAG_BYTES ||
        input_len - PV_TAG_BYTES > plaintext_capacity) {
        return -1;
    }
    size_t ciphertext_len = input_len - PV_TAG_BYTES;
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    int rc = mbedtls_chachapoly_setkey(&ctx, key);
    if (rc == 0) {
        rc = mbedtls_chachapoly_auth_decrypt(&ctx, ciphertext_len, nonce,
                                             NULL, 0U,
                                             input + ciphertext_len, input,
                                             plaintext);
    }
    mbedtls_chachapoly_free(&ctx);
    if (rc == 0) *plaintext_len = ciphertext_len;
    return rc;
}

static airplay_tlv8_result_t read_tlv(const uint8_t *input,
                                     size_t input_len,
                                     uint8_t type,
                                     uint8_t *value,
                                     size_t capacity,
                                     size_t *value_len)
{
    return airplay_tlv8_get(input, input_len, type, value, capacity,
                            value_len);
}

static airplay_pair_verify_result_t handle_m1(
    airplay_pair_verify_t *session,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    uint8_t controller_public[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
    uint8_t response_submessage[PV_SUBMESSAGE_MAX_BYTES];
    uint8_t encrypted[PV_SUBMESSAGE_MAX_BYTES + PV_TAG_BYTES];
    uint8_t signature_input[AIRPLAY_PAIR_VERIFY_KEY_BYTES * 2U +
                            PV_IDENTIFIER_STORAGE_BYTES];
    uint8_t signature[AIRPLAY_PAIR_VERIFY_SIGNATURE_BYTES];
    uint8_t nonce[PV_NONCE_BYTES];
    size_t public_len = 0U;
    size_t submessage_len = 0U;
    size_t encrypted_len = 0U;
    size_t signature_input_len = 0U;
    size_t signature_len = 0U;
    airplay_pair_verify_result_t result = AIRPLAY_PAIR_VERIFY_CRYPTO_FAILED;
    mbedtls_ecp_point accessory_public;
    mbedtls_ecp_point controller_point;
    mbedtls_mpi shared;
    mbedtls_ecp_point_init(&accessory_public);
    mbedtls_ecp_point_init(&controller_point);
    mbedtls_mpi_init(&shared);

    if (read_tlv(request, request_len, PV_PUBLIC_KEY,
                 controller_public, sizeof(controller_public), &public_len) !=
            AIRPLAY_TLV8_OK ||
        public_len != sizeof(controller_public)) {
        result = AIRPLAY_PAIR_VERIFY_AUTH_FAILED;
        goto cleanup;
    }
    if (mbedtls_ecp_gen_keypair(&session->group, &session->private_key,
                                &accessory_public, pv_rng, session) != 0 ||
        mbedtls_ecp_point_write_binary(
            &session->group, &accessory_public,
            MBEDTLS_ECP_PF_UNCOMPRESSED, &public_len,
            session->accessory_ephemeral_public,
            sizeof(session->accessory_ephemeral_public)) != 0 ||
        public_len != sizeof(session->accessory_ephemeral_public) ||
        mbedtls_ecp_point_read_binary(&session->group, &controller_point,
                                      controller_public,
                                      sizeof(controller_public)) != 0 ||
        mbedtls_ecdh_compute_shared(&session->group, &shared,
                                    &controller_point, &session->private_key,
                                    pv_rng, session) != 0 ||
        mbedtls_mpi_write_binary_le(&shared, session->shared_secret,
                                    sizeof(session->shared_secret)) != 0 ||
        derive_key(session->shared_secret, sizeof(session->shared_secret),
                   "Pair-Verify-Encrypt-Salt",
                   "Pair-Verify-Encrypt-Info",
                   session->encryption_key) != 0) {
        result = AIRPLAY_PAIR_VERIFY_CRYPTO_FAILED;
        goto cleanup;
    }

    memcpy(session->controller_ephemeral_public, controller_public,
           sizeof(controller_public));
    memcpy(signature_input, session->accessory_ephemeral_public,
           sizeof(session->accessory_ephemeral_public));
    memcpy(signature_input + sizeof(session->accessory_ephemeral_public),
           session->accessory_identifier,
           session->provider.accessory_identifier_len);
    memcpy(signature_input + sizeof(session->accessory_ephemeral_public) +
               session->provider.accessory_identifier_len,
           session->controller_ephemeral_public,
           sizeof(session->controller_ephemeral_public));
    signature_input_len = sizeof(session->accessory_ephemeral_public) +
                          session->provider.accessory_identifier_len +
                          sizeof(session->controller_ephemeral_public);
    if (!session->provider.sign_accessory(
            signature_input, signature_input_len, signature,
            session->provider.provider_context)) {
        result = AIRPLAY_PAIR_VERIFY_AUTH_FAILED;
        goto cleanup;
    }
    signature_len = sizeof(signature);
    const airplay_tlv8_item_t sub_items[] = {
        {.type = PV_IDENTIFIER,
         .value = session->accessory_identifier,
         .value_len = session->provider.accessory_identifier_len},
        {.type = PV_SIGNATURE, .value = signature, .value_len = signature_len},
    };
    if (airplay_tlv8_encode(sub_items, 2U, response_submessage,
                            sizeof(response_submessage), &submessage_len) !=
        AIRPLAY_TLV8_OK) {
        result = AIRPLAY_PAIR_VERIFY_CRYPTO_FAILED;
        goto cleanup;
    }
    make_nonce("PV-Msg02", nonce);
    if (chacha_encrypt(session->encryption_key, nonce, response_submessage,
                       submessage_len, encrypted, sizeof(encrypted),
                       &encrypted_len) != 0) {
        result = AIRPLAY_PAIR_VERIFY_CRYPTO_FAILED;
        goto cleanup;
    }
    const uint8_t state = 2U;
    const airplay_tlv8_item_t items[] = {
        {.type = PV_STATE, .value = &state, .value_len = 1U},
        {.type = PV_PUBLIC_KEY,
         .value = session->accessory_ephemeral_public,
         .value_len = sizeof(session->accessory_ephemeral_public)},
        {.type = PV_ENCRYPTED_DATA, .value = encrypted,
         .value_len = encrypted_len},
    };
    result = encode_response(items, 3U, response, response_capacity,
                              response_len);
    if (result == AIRPLAY_PAIR_VERIFY_OK) session->state = PV_SESSION_WAIT_M3;

cleanup:
    mbedtls_ecp_point_free(&accessory_public);
    mbedtls_ecp_point_free(&controller_point);
    mbedtls_mpi_free(&shared);
    scrub(controller_public, sizeof(controller_public));
    scrub(response_submessage, sizeof(response_submessage));
    scrub(encrypted, sizeof(encrypted));
    scrub(signature_input, sizeof(signature_input));
    scrub(signature, sizeof(signature));
    scrub(nonce, sizeof(nonce));
    return result;
}

static airplay_pair_verify_result_t handle_m3(
    airplay_pair_verify_t *session,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    uint8_t encrypted[PV_SUBMESSAGE_MAX_BYTES + PV_TAG_BYTES];
    uint8_t submessage[PV_SUBMESSAGE_MAX_BYTES];
    uint8_t controller_identifier[PV_IDENTIFIER_STORAGE_BYTES];
    uint8_t controller_signature[AIRPLAY_PAIR_VERIFY_SIGNATURE_BYTES];
    uint8_t controller_public[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
    uint8_t signature_input[AIRPLAY_PAIR_VERIFY_KEY_BYTES * 2U +
                            PV_IDENTIFIER_STORAGE_BYTES];
    uint8_t nonce[PV_NONCE_BYTES];
    size_t encrypted_len = 0U;
    size_t submessage_len = 0U;
    size_t identifier_len = 0U;
    size_t signature_len = 0U;
    airplay_pair_verify_result_t result = AIRPLAY_PAIR_VERIFY_AUTH_FAILED;

    if (read_tlv(request, request_len, PV_ENCRYPTED_DATA,
                 encrypted, sizeof(encrypted), &encrypted_len) !=
            AIRPLAY_TLV8_OK ||
        encrypted_len < PV_TAG_BYTES) {
        goto cleanup;
    }
    make_nonce("PV-Msg03", nonce);
    if (chacha_decrypt(session->encryption_key, nonce, encrypted,
                       encrypted_len, submessage, sizeof(submessage),
                       &submessage_len) != 0 ||
        read_tlv(submessage, submessage_len, PV_IDENTIFIER,
                 controller_identifier, sizeof(controller_identifier),
                 &identifier_len) != AIRPLAY_TLV8_OK ||
        identifier_len == 0U ||
        read_tlv(submessage, submessage_len, PV_SIGNATURE,
                 controller_signature, sizeof(controller_signature),
                 &signature_len) != AIRPLAY_TLV8_OK ||
        signature_len != sizeof(controller_signature) ||
        !session->provider.lookup_controller(
            controller_identifier, identifier_len, controller_public,
            session->provider.provider_context)) {
        goto cleanup;
    }
    memcpy(signature_input, session->controller_ephemeral_public,
           sizeof(session->controller_ephemeral_public));
    memcpy(signature_input + sizeof(session->controller_ephemeral_public),
           controller_identifier, identifier_len);
    memcpy(signature_input + sizeof(session->controller_ephemeral_public) +
               identifier_len,
           session->accessory_ephemeral_public,
           sizeof(session->accessory_ephemeral_public));
    size_t signature_input_len = sizeof(session->controller_ephemeral_public) +
                                identifier_len +
                                sizeof(session->accessory_ephemeral_public);
    if (!session->provider.verify_controller(
            controller_public, signature_input, signature_input_len,
            controller_signature, session->provider.provider_context)) {
        goto cleanup;
    }

    if (derive_key(session->shared_secret, sizeof(session->shared_secret),
                   "Control-Salt", "Control-Write-Encryption-Key",
                   session->control_keys.read_key) != 0 ||
        derive_key(session->shared_secret, sizeof(session->shared_secret),
                   "Control-Salt", "Control-Read-Encryption-Key",
                   session->control_keys.write_key) != 0 ||
        derive_key(session->shared_secret, sizeof(session->shared_secret),
                   "Events-Salt", "Events-Read-Encryption-Key",
                   session->event_keys.read_key) != 0 ||
        derive_key(session->shared_secret, sizeof(session->shared_secret),
                   "Events-Salt", "Events-Write-Encryption-Key",
                   session->event_keys.write_key) != 0) {
        result = AIRPLAY_PAIR_VERIFY_CRYPTO_FAILED;
        goto cleanup;
    }
    const uint8_t state = 4U;
    const airplay_tlv8_item_t item = {
        .type = PV_STATE, .value = &state, .value_len = 1U,
    };
    result = encode_response(&item, 1U, response, response_capacity,
                             response_len);
    if (result == AIRPLAY_PAIR_VERIFY_OK) {
        session->state = PV_SESSION_COMPLETE;
        scrub(session->encryption_key, sizeof(session->encryption_key));
        mbedtls_mpi_free(&session->private_key);
        mbedtls_mpi_init(&session->private_key);
        scrub(session->accessory_ephemeral_public,
              sizeof(session->accessory_ephemeral_public));
        scrub(session->controller_ephemeral_public,
              sizeof(session->controller_ephemeral_public));
    }

cleanup:
    scrub(encrypted, sizeof(encrypted));
    scrub(submessage, sizeof(submessage));
    scrub(controller_identifier, sizeof(controller_identifier));
    scrub(controller_signature, sizeof(controller_signature));
    scrub(controller_public, sizeof(controller_public));
    scrub(signature_input, sizeof(signature_input));
    scrub(nonce, sizeof(nonce));
    return result;
}

airplay_pair_verify_result_t airplay_pair_verify_create(
    const airplay_pair_verify_config_t *config,
    airplay_pair_verify_t **out_session)
{
    if (out_session == NULL) return AIRPLAY_PAIR_VERIFY_INVALID_ARGUMENT;
    *out_session = NULL;
    if (config == NULL || config->accessory_identifier == NULL ||
        config->accessory_identifier_len == 0U ||
        config->accessory_identifier_len > PV_IDENTIFIER_STORAGE_BYTES ||
        config->random == NULL || config->sign_accessory == NULL ||
        config->verify_controller == NULL ||
        config->lookup_controller == NULL) {
        return AIRPLAY_PAIR_VERIFY_INVALID_ARGUMENT;
    }
    airplay_pair_verify_t *session = pair_verify_calloc();
    if (session == NULL) return AIRPLAY_PAIR_VERIFY_NO_MEMORY;
    session->provider = *config;
    memcpy(session->accessory_identifier, config->accessory_identifier,
           config->accessory_identifier_len);
    session->provider.accessory_identifier = session->accessory_identifier;
    mbedtls_ecp_group_init(&session->group);
    mbedtls_mpi_init(&session->private_key);
    if (mbedtls_ecp_group_load(&session->group,
                               MBEDTLS_ECP_DP_CURVE25519) != 0) {
        mbedtls_ecp_group_free(&session->group);
        mbedtls_mpi_free(&session->private_key);
        scrub(session, sizeof(*session));
        pair_verify_free(session);
        return AIRPLAY_PAIR_VERIFY_CRYPTO_FAILED;
    }
    session->state = PV_SESSION_WAIT_M1;
    *out_session = session;
    return AIRPLAY_PAIR_VERIFY_OK;
}

airplay_pair_verify_result_t airplay_pair_verify_handle(
    airplay_pair_verify_t *session,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    if (session == NULL || request == NULL || request_len == 0U ||
        request_len > AIRPLAY_PAIR_VERIFY_MAX_MESSAGE_BYTES ||
        response == NULL || response_len == NULL) {
        return AIRPLAY_PAIR_VERIFY_INVALID_ARGUMENT;
    }
    *response_len = 0U;
    uint8_t state = 0U;
    size_t state_len = 0U;
    if (read_tlv(request, request_len, PV_STATE, &state, sizeof(state),
                 &state_len) != AIRPLAY_TLV8_OK || state_len != 1U) {
        return fail_session(session, 0U, response, response_capacity,
                             response_len, AIRPLAY_PAIR_VERIFY_AUTH_FAILED);
    }
    if (session->state == PV_SESSION_WAIT_M1 && state == 1U) {
        session->random_failed = false;
        airplay_pair_verify_result_t result = handle_m1(
            session, request, request_len, response, response_capacity,
            response_len);
        if (result != AIRPLAY_PAIR_VERIFY_OK) {
            if (session->random_failed) result = AIRPLAY_PAIR_VERIFY_RANDOM_FAILED;
            return fail_session(session, 2U, response, response_capacity,
                                 response_len, result);
        }
        return result;
    }
    if (session->state == PV_SESSION_WAIT_M3 && state == 3U) {
        airplay_pair_verify_result_t result = handle_m3(
            session, request, request_len, response, response_capacity,
            response_len);
        if (result != AIRPLAY_PAIR_VERIFY_OK) {
            return fail_session(session, 4U, response, response_capacity,
                                 response_len, result);
        }
        return result;
    }
    if (session->state == PV_SESSION_COMPLETE ||
        session->state == PV_SESSION_FAILED) {
        return encode_error(state, response, response_capacity, response_len);
    }
    return fail_session(session, state, response, response_capacity,
                         response_len, AIRPLAY_PAIR_VERIFY_STATE_ERROR);
}

airplay_pair_verify_result_t airplay_pair_verify_get_control_keys(
    const airplay_pair_verify_t *session,
    airplay_pair_verify_control_keys_t *out_keys)
{
    if (session == NULL || out_keys == NULL) {
        return AIRPLAY_PAIR_VERIFY_INVALID_ARGUMENT;
    }
    if (session->state != PV_SESSION_COMPLETE) {
        memset(out_keys, 0, sizeof(*out_keys));
        return AIRPLAY_PAIR_VERIFY_STATE_ERROR;
    }
    memcpy(out_keys, &session->control_keys, sizeof(*out_keys));
    return AIRPLAY_PAIR_VERIFY_OK;
}

airplay_pair_verify_result_t airplay_pair_verify_get_event_keys(
    const airplay_pair_verify_t *session,
    airplay_pair_verify_event_keys_t *out_keys)
{
    if (session == NULL || out_keys == NULL) {
        return AIRPLAY_PAIR_VERIFY_INVALID_ARGUMENT;
    }
    if (session->state != PV_SESSION_COMPLETE) {
        memset(out_keys, 0, sizeof(*out_keys));
        return AIRPLAY_PAIR_VERIFY_STATE_ERROR;
    }
    memcpy(out_keys, &session->event_keys, sizeof(*out_keys));
    return AIRPLAY_PAIR_VERIFY_OK;
}

static airplay_pair_verify_result_t derive_stream_key(
    const airplay_pair_verify_t *session,
    uint64_t stream_connection_id,
    uint8_t output_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES], const char *label)
{
    if (output_key != NULL) memset(output_key, 0, AIRPLAY_PAIR_VERIFY_KEY_BYTES);
    if (session == NULL || output_key == NULL) {
        return AIRPLAY_PAIR_VERIFY_INVALID_ARGUMENT;
    }
    if (session->state != PV_SESSION_COMPLETE) {
        return AIRPLAY_PAIR_VERIFY_STATE_ERROR;
    }
    char salt[48];
    int salt_len = snprintf(salt, sizeof(salt), "DataStream-Salt%" PRIu64,
                            stream_connection_id);
    if (salt_len <= 0 || (size_t)salt_len >= sizeof(salt)) {
        scrub(salt, sizeof(salt));
        return AIRPLAY_PAIR_VERIFY_CRYPTO_FAILED;
    }
    const mbedtls_md_info_t *sha512 =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    int rc = sha512 == NULL ? -1 : mbedtls_hkdf(
        sha512, (const uint8_t *)salt, (size_t)salt_len,
        session->shared_secret, sizeof(session->shared_secret),
        (const uint8_t *)label, strlen(label),
        output_key, AIRPLAY_PAIR_VERIFY_KEY_BYTES);
    scrub(salt, sizeof(salt));
    if (rc != 0) {
        scrub(output_key, AIRPLAY_PAIR_VERIFY_KEY_BYTES);
        return AIRPLAY_PAIR_VERIFY_CRYPTO_FAILED;
    }
    return AIRPLAY_PAIR_VERIFY_OK;
}

airplay_pair_verify_result_t airplay_pair_verify_derive_screen_output_key(
    const airplay_pair_verify_t *s, uint64_t id, uint8_t key[AIRPLAY_PAIR_VERIFY_KEY_BYTES])
{ return derive_stream_key(s, id, key, "DataStream-Output-Encryption-Key"); }
airplay_pair_verify_result_t airplay_pair_verify_derive_input_key(
    const airplay_pair_verify_t *s, uint64_t id, uint8_t key[AIRPLAY_PAIR_VERIFY_KEY_BYTES])
{ return derive_stream_key(s, id, key, "DataStream-Input-Encryption-Key"); }

void airplay_pair_verify_destroy(airplay_pair_verify_t *session)
{
    if (session == NULL) return;
    mbedtls_ecp_group_free(&session->group);
    mbedtls_mpi_free(&session->private_key);
    scrub(session, sizeof(*session));
    pair_verify_free(session);
}
