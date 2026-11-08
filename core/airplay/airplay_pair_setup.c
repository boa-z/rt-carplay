/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pair-Setup message flow follows DiPlay PairSetup.kt and AirPlayCrypto.kt.
 * See THIRD_PARTY_NOTICES.md. Credentials, Ed25519 operations, and pairing
 * storage are delegated to the configured platform provider.
 */
#include "airplay_pair_setup.h"

#include <stdlib.h>
#include <string.h>

#include "airplay_srp6a.h"
#include "airplay_tlv8.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

#define PS_METHOD 0x00U
#define PS_IDENTIFIER 0x01U
#define PS_SALT 0x02U
#define PS_PUBLIC_KEY 0x03U
#define PS_PROOF 0x04U
#define PS_ENCRYPTED_DATA 0x05U
#define PS_STATE 0x06U
#define PS_ERROR 0x07U
#define PS_SIGNATURE 0x0aU
#define PS_ERROR_AUTHENTICATION 2U
#define PS_SRP_USERNAME "Pair-Setup"
#define PS_SALT_BYTES AIRPLAY_SRP6A_SALT_BYTES
#define PS_SRP_PUBLIC_BYTES AIRPLAY_SRP6A_PUBLIC_BYTES
#define PS_PROOF_BYTES AIRPLAY_SRP6A_PROOF_BYTES
#define PS_SESSION_KEY_BYTES AIRPLAY_SRP6A_SESSION_KEY_BYTES
#define PS_NONCE_BYTES 12U
#define PS_TAG_BYTES 16U
#define PS_SUBMESSAGE_MAX_BYTES 256U
#define PS_ENCRYPTED_MAX_BYTES (PS_SUBMESSAGE_MAX_BYTES + PS_TAG_BYTES)

typedef enum {
    PS_SESSION_WAIT_M1 = 0,
    PS_SESSION_WAIT_M3,
    PS_SESSION_WAIT_M5,
    PS_SESSION_COMPLETE,
    PS_SESSION_FAILED,
} ps_session_state_t;

struct airplay_pair_setup {
    airplay_pair_setup_config_t provider;
    uint8_t accessory_identifier[AIRPLAY_PAIR_SETUP_MAX_IDENTIFIER_BYTES];
    uint8_t accessory_public_key[AIRPLAY_PAIR_SETUP_KEY_BYTES];
    char pairing_code[AIRPLAY_PAIR_SETUP_MAX_PAIRING_CODE_BYTES + 1U];
    airplay_srp6a_server_t *srp;
    uint8_t session_key[PS_SESSION_KEY_BYTES];
    uint8_t encryption_key[AIRPLAY_PAIR_SETUP_KEY_BYTES];
    ps_session_state_t state;
    bool random_failed;
};

static airplay_pair_setup_t *pair_setup_calloc(void)
{
    return calloc(1U, sizeof(airplay_pair_setup_t));
}

static void pair_setup_free(airplay_pair_setup_t *session)
{
    free(session);
}

static void scrub(void *buffer, size_t length)
{
    if (buffer != NULL && length != 0U) {
        mbedtls_platform_zeroize(buffer, length);
    }
}

static bool srp_random(uint8_t *output, size_t output_len, void *context)
{
    airplay_pair_setup_t *session = context;
    if (session == NULL || output == NULL ||
        !session->provider.random(output, output_len,
                                 session->provider.provider_context)) {
        if (session != NULL) session->random_failed = true;
        return false;
    }
    return true;
}

static airplay_pair_setup_result_t encode_error(
    uint8_t state,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    const uint8_t error = PS_ERROR_AUTHENTICATION;
    const airplay_tlv8_item_t items[] = {
        {.type = PS_STATE, .value = &state, .value_len = 1U},
        {.type = PS_ERROR, .value = &error, .value_len = 1U},
    };
    return airplay_tlv8_encode(items, 2U, response, response_capacity,
                               response_len) == AIRPLAY_TLV8_OK
               ? AIRPLAY_PAIR_SETUP_AUTH_FAILED
               : AIRPLAY_PAIR_SETUP_BUFFER_TOO_SMALL;
}

static airplay_pair_setup_result_t encode_items(
    const airplay_tlv8_item_t *items,
    size_t item_count,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    airplay_tlv8_result_t result = airplay_tlv8_encode(
        items, item_count, response, response_capacity, response_len);
    if (result == AIRPLAY_TLV8_OK) return AIRPLAY_PAIR_SETUP_OK;
    return result == AIRPLAY_TLV8_TOO_LARGE
               ? AIRPLAY_PAIR_SETUP_BUFFER_TOO_SMALL
               : AIRPLAY_PAIR_SETUP_INVALID_ARGUMENT;
}

static airplay_pair_setup_result_t fail_session(
    airplay_pair_setup_t *session,
    uint8_t state,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len,
    airplay_pair_setup_result_t reason)
{
    session->state = PS_SESSION_FAILED;
    if (session->srp != NULL) {
        airplay_srp6a_server_destroy(session->srp);
        session->srp = NULL;
    }
    scrub(session->session_key, sizeof(session->session_key));
    scrub(session->encryption_key, sizeof(session->encryption_key));
    return encode_error(state, response, response_capacity, response_len) ==
                   AIRPLAY_PAIR_SETUP_AUTH_FAILED
               ? reason
               : AIRPLAY_PAIR_SETUP_BUFFER_TOO_SMALL;
}

static int derive_key(const uint8_t *input,
                      size_t input_len,
                      const char *salt,
                      const char *info,
                      uint8_t output[AIRPLAY_PAIR_SETUP_KEY_BYTES])
{
    const mbedtls_md_info_t *sha512 =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    if (sha512 == NULL) return -1;
    return mbedtls_hkdf(sha512, (const uint8_t *)salt, strlen(salt),
                        input, input_len, (const uint8_t *)info, strlen(info),
                        output, AIRPLAY_PAIR_SETUP_KEY_BYTES);
}

static void make_nonce(const char label[8], uint8_t nonce[PS_NONCE_BYTES])
{
    memset(nonce, 0, PS_NONCE_BYTES);
    memcpy(nonce + 4U, label, 8U);
}

static int encrypt_message(const uint8_t key[AIRPLAY_PAIR_SETUP_KEY_BYTES],
                           const uint8_t nonce[PS_NONCE_BYTES],
                           const uint8_t *plaintext,
                           size_t plaintext_len,
                           uint8_t *output,
                           size_t output_capacity,
                           size_t *output_len)
{
    if (plaintext_len > output_capacity ||
        PS_TAG_BYTES > output_capacity - plaintext_len) return -1;
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    int rc = mbedtls_chachapoly_setkey(&ctx, key);
    if (rc == 0) {
        rc = mbedtls_chachapoly_encrypt_and_tag(
            &ctx, plaintext_len, nonce, NULL, 0U, plaintext, output,
            output + plaintext_len);
    }
    mbedtls_chachapoly_free(&ctx);
    if (rc == 0) *output_len = plaintext_len + PS_TAG_BYTES;
    return rc;
}

static int decrypt_message(const uint8_t key[AIRPLAY_PAIR_SETUP_KEY_BYTES],
                           const uint8_t nonce[PS_NONCE_BYTES],
                           const uint8_t *input,
                           size_t input_len,
                           uint8_t *plaintext,
                           size_t plaintext_capacity,
                           size_t *plaintext_len)
{
    if (input_len < PS_TAG_BYTES ||
        input_len - PS_TAG_BYTES > plaintext_capacity) return -1;
    size_t ciphertext_len = input_len - PS_TAG_BYTES;
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    int rc = mbedtls_chachapoly_setkey(&ctx, key);
    if (rc == 0) {
        rc = mbedtls_chachapoly_auth_decrypt(
            &ctx, ciphertext_len, nonce, NULL, 0U,
            input + ciphertext_len, input, plaintext);
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

static airplay_pair_setup_result_t handle_m1(
    airplay_pair_setup_t *session,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    uint8_t salt[PS_SALT_BYTES];
    uint8_t public_b[PS_SRP_PUBLIC_BYTES];
    session->random_failed = false;
    airplay_srp6a_result_t srp_result = airplay_srp6a_server_start(
        PS_SRP_USERNAME, session->pairing_code, srp_random, session,
        &session->srp, salt, public_b);
    if (srp_result != AIRPLAY_SRP6A_OK) {
        scrub(salt, sizeof(salt));
        scrub(public_b, sizeof(public_b));
        return session->random_failed ? AIRPLAY_PAIR_SETUP_RANDOM_FAILED
                                      : AIRPLAY_PAIR_SETUP_CRYPTO_FAILED;
    }
    const uint8_t state = 2U;
    const airplay_tlv8_item_t items[] = {
        {.type = PS_STATE, .value = &state, .value_len = 1U},
        {.type = PS_PUBLIC_KEY, .value = public_b,
         .value_len = sizeof(public_b)},
        {.type = PS_SALT, .value = salt, .value_len = sizeof(salt)},
    };
    airplay_pair_setup_result_t result = encode_items(
        items, 3U, response, response_capacity, response_len);
    if (result == AIRPLAY_PAIR_SETUP_OK) session->state = PS_SESSION_WAIT_M3;
    scrub(salt, sizeof(salt));
    scrub(public_b, sizeof(public_b));
    return result;
}

static airplay_pair_setup_result_t handle_m3(
    airplay_pair_setup_t *session,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    uint8_t public_a[PS_SRP_PUBLIC_BYTES];
    uint8_t proof_m1[PS_PROOF_BYTES];
    uint8_t proof_m2[PS_PROOF_BYTES];
    size_t public_a_len = 0U;
    size_t proof_m1_len = 0U;
    if (session->srp == NULL ||
        read_tlv(request, request_len, PS_PUBLIC_KEY, public_a,
                 sizeof(public_a), &public_a_len) != AIRPLAY_TLV8_OK ||
        public_a_len != sizeof(public_a) ||
        read_tlv(request, request_len, PS_PROOF, proof_m1,
                 sizeof(proof_m1), &proof_m1_len) != AIRPLAY_TLV8_OK ||
        proof_m1_len != sizeof(proof_m1)) {
        scrub(public_a, sizeof(public_a));
        scrub(proof_m1, sizeof(proof_m1));
        return AIRPLAY_PAIR_SETUP_AUTH_FAILED;
    }
    airplay_srp6a_result_t srp_result = airplay_srp6a_server_verify(
        session->srp, public_a, public_a_len, proof_m1, proof_m1_len,
        session->session_key, proof_m2);
    airplay_srp6a_server_destroy(session->srp);
    session->srp = NULL;
    scrub(public_a, sizeof(public_a));
    scrub(proof_m1, sizeof(proof_m1));
    if (srp_result != AIRPLAY_SRP6A_OK) {
        scrub(proof_m2, sizeof(proof_m2));
        return srp_result == AIRPLAY_SRP6A_AUTH_FAILED
                   ? AIRPLAY_PAIR_SETUP_AUTH_FAILED
                   : AIRPLAY_PAIR_SETUP_CRYPTO_FAILED;
    }
    const uint8_t state = 4U;
    const airplay_tlv8_item_t items[] = {
        {.type = PS_STATE, .value = &state, .value_len = 1U},
        {.type = PS_PROOF, .value = proof_m2, .value_len = sizeof(proof_m2)},
    };
    airplay_pair_setup_result_t result = encode_items(
        items, 2U, response, response_capacity, response_len);
    if (result == AIRPLAY_PAIR_SETUP_OK) session->state = PS_SESSION_WAIT_M5;
    scrub(proof_m2, sizeof(proof_m2));
    return result;
}

static airplay_pair_setup_result_t handle_m5(
    airplay_pair_setup_t *session,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    uint8_t encrypted[PS_ENCRYPTED_MAX_BYTES];
    uint8_t plaintext[PS_SUBMESSAGE_MAX_BYTES];
    uint8_t controller_identifier[AIRPLAY_PAIR_SETUP_MAX_IDENTIFIER_BYTES];
    uint8_t controller_public[AIRPLAY_PAIR_SETUP_KEY_BYTES];
    uint8_t controller_signature[AIRPLAY_PAIR_SETUP_SIGNATURE_BYTES];
    uint8_t accessory_signature[ AIRPLAY_PAIR_SETUP_SIGNATURE_BYTES ];
    uint8_t signing_key[AIRPLAY_PAIR_SETUP_KEY_BYTES];
    uint8_t signature_input[AIRPLAY_PAIR_SETUP_KEY_BYTES +
                            AIRPLAY_PAIR_SETUP_MAX_IDENTIFIER_BYTES +
                            AIRPLAY_PAIR_SETUP_KEY_BYTES];
    uint8_t nonce[PS_NONCE_BYTES];
    uint8_t response_submessage[PS_SUBMESSAGE_MAX_BYTES];
    uint8_t response_encrypted[PS_ENCRYPTED_MAX_BYTES];
    uint8_t response_tlv[512];
    size_t encrypted_len = 0U;
    size_t plaintext_len = 0U;
    size_t identifier_len = 0U;
    size_t public_len = 0U;
    size_t signature_len = 0U;
    size_t response_submessage_len = 0U;
    size_t response_encrypted_len = 0U;
    size_t response_tlv_len = 0U;
    size_t signature_input_len = 0U;
    airplay_pair_setup_result_t result = AIRPLAY_PAIR_SETUP_AUTH_FAILED;

    if (read_tlv(request, request_len, PS_ENCRYPTED_DATA,
                 encrypted, sizeof(encrypted), &encrypted_len) !=
            AIRPLAY_TLV8_OK ||
        derive_key(session->session_key, sizeof(session->session_key),
                   "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info",
                   session->encryption_key) != 0) {
        result = AIRPLAY_PAIR_SETUP_CRYPTO_FAILED;
        goto cleanup;
    }
    make_nonce("PS-Msg05", nonce);
    if (decrypt_message(session->encryption_key, nonce, encrypted,
                        encrypted_len, plaintext, sizeof(plaintext),
                        &plaintext_len) != 0 ||
        read_tlv(plaintext, plaintext_len, PS_IDENTIFIER,
                 controller_identifier, sizeof(controller_identifier),
                 &identifier_len) != AIRPLAY_TLV8_OK ||
        identifier_len == 0U ||
        read_tlv(plaintext, plaintext_len, PS_PUBLIC_KEY,
                 controller_public, sizeof(controller_public),
                 &public_len) != AIRPLAY_TLV8_OK ||
        public_len != sizeof(controller_public) ||
        read_tlv(plaintext, plaintext_len, PS_SIGNATURE,
                 controller_signature, sizeof(controller_signature),
                 &signature_len) != AIRPLAY_TLV8_OK ||
        signature_len != sizeof(controller_signature)) {
        goto cleanup;
    }

    if (derive_key(session->session_key, sizeof(session->session_key),
                   "Pair-Setup-Controller-Sign-Salt",
                   "Pair-Setup-Controller-Sign-Info", signing_key) != 0) {
        result = AIRPLAY_PAIR_SETUP_CRYPTO_FAILED;
        goto cleanup;
    }
    memcpy(signature_input, signing_key, sizeof(signing_key));
    memcpy(signature_input + sizeof(signing_key), controller_identifier,
           identifier_len);
    memcpy(signature_input + sizeof(signing_key) + identifier_len,
           controller_public, sizeof(controller_public));
    signature_input_len = sizeof(signing_key) + identifier_len +
                          sizeof(controller_public);
    if (!session->provider.verify_controller(
            controller_public, signature_input, signature_input_len,
            controller_signature, session->provider.provider_context)) {
        goto cleanup;
    }
    if (derive_key(session->session_key, sizeof(session->session_key),
                   "Pair-Setup-Accessory-Sign-Salt",
                   "Pair-Setup-Accessory-Sign-Info", signing_key) != 0) {
        result = AIRPLAY_PAIR_SETUP_CRYPTO_FAILED;
        goto cleanup;
    }
    memcpy(signature_input, signing_key, sizeof(signing_key));
    memcpy(signature_input + sizeof(signing_key), session->accessory_identifier,
           session->provider.accessory_identifier_len);
    memcpy(signature_input + sizeof(signing_key) +
               session->provider.accessory_identifier_len,
           session->accessory_public_key, sizeof(session->accessory_public_key));
    signature_input_len = sizeof(signing_key) +
                          session->provider.accessory_identifier_len +
                          sizeof(session->accessory_public_key);
    if (!session->provider.sign_accessory(
            signature_input, signature_input_len, accessory_signature,
            session->provider.provider_context)) {
        result = AIRPLAY_PAIR_SETUP_AUTH_FAILED;
        goto cleanup;
    }
    const airplay_tlv8_item_t sub_items[] = {
        {.type = PS_IDENTIFIER, .value = session->accessory_identifier,
         .value_len = session->provider.accessory_identifier_len},
        {.type = PS_PUBLIC_KEY, .value = session->accessory_public_key,
         .value_len = sizeof(session->accessory_public_key)},
        {.type = PS_SIGNATURE, .value = accessory_signature,
         .value_len = sizeof(accessory_signature)},
    };
    if (airplay_tlv8_encode(sub_items, 3U, response_submessage,
                            sizeof(response_submessage),
                            &response_submessage_len) != AIRPLAY_TLV8_OK) {
        result = AIRPLAY_PAIR_SETUP_CRYPTO_FAILED;
        goto cleanup;
    }
    make_nonce("PS-Msg06", nonce);
    if (encrypt_message(session->encryption_key, nonce, response_submessage,
                        response_submessage_len, response_encrypted,
                        sizeof(response_encrypted),
                        &response_encrypted_len) != 0) {
        result = AIRPLAY_PAIR_SETUP_CRYPTO_FAILED;
        goto cleanup;
    }
    const uint8_t state = 6U;
    const airplay_tlv8_item_t items[] = {
        {.type = PS_STATE, .value = &state, .value_len = 1U},
        {.type = PS_ENCRYPTED_DATA, .value = response_encrypted,
         .value_len = response_encrypted_len},
    };
    if (airplay_tlv8_encode(items, 2U, response_tlv, sizeof(response_tlv),
                            &response_tlv_len) != AIRPLAY_TLV8_OK) {
        result = AIRPLAY_PAIR_SETUP_CRYPTO_FAILED;
        goto cleanup;
    }
    if (response_tlv_len > response_capacity) {
        result = AIRPLAY_PAIR_SETUP_BUFFER_TOO_SMALL;
        goto cleanup;
    }
    if (!session->provider.save_pairing(
            controller_identifier, identifier_len, controller_public,
            session->provider.provider_context)) {
        result = AIRPLAY_PAIR_SETUP_STORAGE_FAILED;
        goto cleanup;
    }
    memcpy(response, response_tlv, response_tlv_len);
    *response_len = response_tlv_len;
    result = AIRPLAY_PAIR_SETUP_OK;
    if (result == AIRPLAY_PAIR_SETUP_OK) {
        session->state = PS_SESSION_COMPLETE;
        scrub(session->session_key, sizeof(session->session_key));
        scrub(session->encryption_key, sizeof(session->encryption_key));
        scrub(session->pairing_code, sizeof(session->pairing_code));
    }

cleanup:
    scrub(encrypted, sizeof(encrypted));
    scrub(plaintext, sizeof(plaintext));
    scrub(controller_identifier, sizeof(controller_identifier));
    scrub(controller_public, sizeof(controller_public));
    scrub(controller_signature, sizeof(controller_signature));
    scrub(accessory_signature, sizeof(accessory_signature));
    scrub(signing_key, sizeof(signing_key));
    scrub(signature_input, sizeof(signature_input));
    scrub(nonce, sizeof(nonce));
    scrub(response_submessage, sizeof(response_submessage));
    scrub(response_encrypted, sizeof(response_encrypted));
    scrub(response_tlv, sizeof(response_tlv));
    return result;
}

airplay_pair_setup_result_t airplay_pair_setup_create(
    const airplay_pair_setup_config_t *config,
    airplay_pair_setup_t **out_session)
{
    if (out_session == NULL) return AIRPLAY_PAIR_SETUP_INVALID_ARGUMENT;
    *out_session = NULL;
    if (config == NULL || config->pairing_code == NULL ||
        config->accessory_identifier == NULL ||
        config->accessory_public_key == NULL || config->random == NULL ||
        config->sign_accessory == NULL || config->verify_controller == NULL ||
        config->save_pairing == NULL || config->accessory_identifier_len == 0U ||
        config->accessory_identifier_len > AIRPLAY_PAIR_SETUP_MAX_IDENTIFIER_BYTES) {
        return AIRPLAY_PAIR_SETUP_INVALID_ARGUMENT;
    }
    size_t pairing_code_len = 0U;
    while (pairing_code_len <= AIRPLAY_PAIR_SETUP_MAX_PAIRING_CODE_BYTES &&
           config->pairing_code[pairing_code_len] != '\0') {
        ++pairing_code_len;
    }
    if (pairing_code_len == 0U ||
        pairing_code_len > AIRPLAY_PAIR_SETUP_MAX_PAIRING_CODE_BYTES) {
        return AIRPLAY_PAIR_SETUP_INVALID_ARGUMENT;
    }
    airplay_pair_setup_t *session = pair_setup_calloc();
    if (session == NULL) return AIRPLAY_PAIR_SETUP_NO_MEMORY;
    session->provider = *config;
    memcpy(session->accessory_identifier, config->accessory_identifier,
           config->accessory_identifier_len);
    session->provider.accessory_identifier = session->accessory_identifier;
    memcpy(session->accessory_public_key, config->accessory_public_key,
           sizeof(session->accessory_public_key));
    memcpy(session->pairing_code, config->pairing_code, pairing_code_len);
    session->state = PS_SESSION_WAIT_M1;
    *out_session = session;
    return AIRPLAY_PAIR_SETUP_OK;
}

airplay_pair_setup_result_t airplay_pair_setup_handle(
    airplay_pair_setup_t *session,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    if (session == NULL || request == NULL || request_len == 0U ||
        request_len > AIRPLAY_PAIR_SETUP_MAX_MESSAGE_BYTES ||
        response == NULL || response_len == NULL) {
        return AIRPLAY_PAIR_SETUP_INVALID_ARGUMENT;
    }
    *response_len = 0U;
    uint8_t state = 0U;
    size_t state_len = 0U;
    if (read_tlv(request, request_len, PS_STATE, &state, sizeof(state),
                 &state_len) != AIRPLAY_TLV8_OK || state_len != 1U) {
        return fail_session(session, 0U, response, response_capacity,
                             response_len, AIRPLAY_PAIR_SETUP_AUTH_FAILED);
    }
    if (session->state == PS_SESSION_WAIT_M1 && state == 1U) {
        airplay_pair_setup_result_t result = handle_m1(
            session, response, response_capacity, response_len);
        if (result != AIRPLAY_PAIR_SETUP_OK) {
            return fail_session(session, 2U, response, response_capacity,
                                 response_len, result);
        }
        return result;
    }
    if (session->state == PS_SESSION_WAIT_M3 && state == 3U) {
        airplay_pair_setup_result_t result = handle_m3(
            session, request, request_len, response, response_capacity,
            response_len);
        if (result != AIRPLAY_PAIR_SETUP_OK) {
            return fail_session(session, 4U, response, response_capacity,
                                 response_len, result);
        }
        return result;
    }
    if (session->state == PS_SESSION_WAIT_M5 && state == 5U) {
        airplay_pair_setup_result_t result = handle_m5(
            session, request, request_len, response, response_capacity,
            response_len);
        if (result != AIRPLAY_PAIR_SETUP_OK) {
            return fail_session(session, 6U, response, response_capacity,
                                 response_len, result);
        }
        return result;
    }
    if (session->state == PS_SESSION_COMPLETE ||
        session->state == PS_SESSION_FAILED) {
        return encode_error(state, response, response_capacity, response_len);
    }
    return fail_session(session, state, response, response_capacity,
                         response_len, AIRPLAY_PAIR_SETUP_STATE_ERROR);
}

bool airplay_pair_setup_is_complete(const airplay_pair_setup_t *session)
{
    return session != NULL && session->state == PS_SESSION_COMPLETE;
}

void airplay_pair_setup_destroy(airplay_pair_setup_t *session)
{
    if (session == NULL) return;
    if (session->srp != NULL) airplay_srp6a_server_destroy(session->srp);
    scrub(session, sizeof(*session));
    pair_setup_free(session);
}
