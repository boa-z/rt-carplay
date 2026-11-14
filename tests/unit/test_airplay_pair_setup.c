/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_pair_setup.h"
#include "airplay_srp6a_test_client.h"
#include "airplay_tlv8.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/chachapoly.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

#define TEST_KEY_BYTES AIRPLAY_PAIR_SETUP_KEY_BYTES
#define TEST_SIG_BYTES AIRPLAY_PAIR_SETUP_SIGNATURE_BYTES
#define TEST_ID "controller-test"
#define TEST_ACCESSORY_ID "accessory-test"
#define TEST_CODE "test-only-code"
#define TEST_PLAINTEXT_MAX 512U

static const uint8_t ACCESSORY_TEST_PUBLIC_KEY[TEST_KEY_BYTES] = {
    0x41,0x43,0x43,0x45,0x53,0x53,0x4f,0x52,
    0x59,0x2d,0x54,0x45,0x53,0x54,0x2d,0x50,
    0x55,0x42,0x4c,0x49,0x43,0x2d,0x4b,0x45,
    0x59,0x2d,0x30,0x30,0x30,0x30,0x30,0x31,
};

typedef struct {
    uint8_t random_counter;
    bool fail_random;
    bool fail_verify;
    bool fail_save;
    uint8_t controller_key[TEST_KEY_BYTES];
    uint8_t accessory_secret[TEST_KEY_BYTES];
    unsigned save_calls;
    uint8_t saved_identifier[AIRPLAY_PAIR_SETUP_MAX_IDENTIFIER_BYTES];
    size_t saved_identifier_len;
    uint8_t saved_public_key[TEST_KEY_BYTES];
} provider_fixture_t;

static bool test_random(uint8_t *output, size_t output_len, void *context)
{
    provider_fixture_t *fixture = context;
    if (fixture->fail_random) return false;
    for (size_t i = 0; i < output_len; ++i) {
        output[i] = fixture->random_counter++;
    }
    return true;
}

/* Test-only signature shim. This is not Ed25519 and is never used by firmware. */
static bool fake_signature(const uint8_t key[TEST_KEY_BYTES],
                           const uint8_t *message,
                           size_t message_len,
                           uint8_t signature[TEST_SIG_BYTES])
{
    const mbedtls_md_info_t *sha512 =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    if (sha512 == NULL) return false;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, sha512, 0);
    if (rc == 0) rc = mbedtls_md_starts(&ctx);
    if (rc == 0) rc = mbedtls_md_update(&ctx, key, TEST_KEY_BYTES);
    if (rc == 0 && message_len != 0U) {
        rc = mbedtls_md_update(&ctx, message, message_len);
    }
    if (rc == 0) rc = mbedtls_md_finish(&ctx, signature);
    mbedtls_md_free(&ctx);
    return rc == 0;
}

static bool sign_accessory(const uint8_t *message,
                           size_t message_len,
                           uint8_t signature[TEST_SIG_BYTES],
                           void *context)
{
    provider_fixture_t *fixture = context;
    return fake_signature(fixture->accessory_secret, message, message_len,
                          signature);
}

static bool verify_controller(const uint8_t public_key[TEST_KEY_BYTES],
                              const uint8_t *message,
                              size_t message_len,
                              const uint8_t signature[TEST_SIG_BYTES],
                              void *context)
{
    provider_fixture_t *fixture = context;
    uint8_t expected[TEST_SIG_BYTES];
    if (fixture->fail_verify ||
        memcmp(public_key, fixture->controller_key, TEST_KEY_BYTES) != 0 ||
        !fake_signature(public_key, message, message_len, expected)) {
        return false;
    }
    uint8_t diff = 0U;
    for (size_t i = 0; i < sizeof(expected); ++i) diff |= expected[i] ^ signature[i];
    mbedtls_platform_zeroize(expected, sizeof(expected));
    return diff == 0U;
}

static bool save_pairing(const uint8_t *identifier,
                         size_t identifier_len,
                         const uint8_t public_key[TEST_KEY_BYTES],
                         void *context)
{
    provider_fixture_t *fixture = context;
    ++fixture->save_calls;
    if (fixture->fail_save ||
        identifier_len > sizeof(fixture->saved_identifier)) return false;
    memcpy(fixture->saved_identifier, identifier, identifier_len);
    fixture->saved_identifier_len = identifier_len;
    memcpy(fixture->saved_public_key, public_key, TEST_KEY_BYTES);
    return true;
}

static void nonce_for(const char label[8], uint8_t nonce[12])
{
    memset(nonce, 0, 12U);
    memcpy(nonce + 4U, label, 8U);
}

static size_t encode_tlv(const airplay_tlv8_item_t *items,
                         size_t item_count,
                         uint8_t *output,
                         size_t capacity)
{
    size_t written = 0U;
    assert(airplay_tlv8_encode(items, item_count, output, capacity, &written) ==
           AIRPLAY_TLV8_OK);
    return written;
}

static size_t get_tlv(const uint8_t *input,
                      size_t input_len,
                      uint8_t type,
                      uint8_t *output,
                      size_t capacity)
{
    size_t written = 0U;
    assert(airplay_tlv8_get(input, input_len, type, output, capacity,
                            &written) == AIRPLAY_TLV8_OK);
    return written;
}

static void derive_key(const uint8_t *input,
                       size_t input_len,
                       const char *salt,
                       const char *info,
                       uint8_t output[TEST_KEY_BYTES])
{
    const mbedtls_md_info_t *sha512 =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    assert(sha512 != NULL);
    assert(mbedtls_hkdf(sha512, (const uint8_t *)salt, strlen(salt),
                        input, input_len, (const uint8_t *)info, strlen(info),
                        output, TEST_KEY_BYTES) == 0);
}

static void encrypt_message(const uint8_t key[TEST_KEY_BYTES],
                            const char label[8],
                            const uint8_t *plaintext,
                            size_t plaintext_len,
                            uint8_t *output,
                            size_t *output_len)
{
    uint8_t nonce[12];
    nonce_for(label, nonce);
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    assert(mbedtls_chachapoly_setkey(&ctx, key) == 0);
    assert(mbedtls_chachapoly_encrypt_and_tag(
               &ctx, plaintext_len, nonce, NULL, 0U, plaintext, output,
               output + plaintext_len) == 0);
    *output_len = plaintext_len + 16U;
    mbedtls_chachapoly_free(&ctx);
    mbedtls_platform_zeroize(nonce, sizeof(nonce));
}

static size_t decrypt_message(const uint8_t key[TEST_KEY_BYTES],
                              const char label[8],
                              const uint8_t *input,
                              size_t input_len,
                              uint8_t *plaintext,
                              size_t plaintext_capacity)
{
    assert(input_len >= 16U && input_len - 16U <= plaintext_capacity);
    uint8_t nonce[12];
    nonce_for(label, nonce);
    size_t ciphertext_len = input_len - 16U;
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    assert(mbedtls_chachapoly_setkey(&ctx, key) == 0);
    assert(mbedtls_chachapoly_auth_decrypt(
               &ctx, ciphertext_len, nonce, NULL, 0U,
               input + ciphertext_len, input, plaintext) == 0);
    mbedtls_chachapoly_free(&ctx);
    mbedtls_platform_zeroize(nonce, sizeof(nonce));
    return ciphertext_len;
}

static void setup_config(provider_fixture_t *fixture,
                         airplay_pair_setup_config_t *config)
{
    *config = (airplay_pair_setup_config_t){
        .pairing_code = TEST_CODE,
        .accessory_identifier = (const uint8_t *)TEST_ACCESSORY_ID,
        .accessory_identifier_len = sizeof(TEST_ACCESSORY_ID) - 1U,
        .accessory_public_key = ACCESSORY_TEST_PUBLIC_KEY,
        .random = test_random,
        .sign_accessory = sign_accessory,
        .verify_controller = verify_controller,
        .save_pairing = save_pairing,
        .provider_context = fixture,
    };
}

static void run_exchange(bool corrupt_srp_proof,
                         bool corrupt_controller_signature,
                         bool fail_storage)
{
    static const char username[] = "Pair-Setup";
    provider_fixture_t fixture = {0};
    for (size_t i = 0; i < sizeof(fixture.controller_key); ++i) {
        fixture.controller_key[i] = (uint8_t)(0x31U + i);
        fixture.accessory_secret[i] = (uint8_t)(0xb0U + i);
    }
    fixture.fail_save = fail_storage;
    airplay_pair_setup_config_t config;
    setup_config(&fixture, &config);
    airplay_pair_setup_t *session = NULL;
    assert(airplay_pair_setup_create(&config, &session) == AIRPLAY_PAIR_SETUP_OK);

    uint8_t request[AIRPLAY_PAIR_SETUP_MAX_MESSAGE_BYTES];
    uint8_t response[AIRPLAY_PAIR_SETUP_MAX_MESSAGE_BYTES];
    uint8_t public_b[AIRPLAY_SRP6A_PUBLIC_BYTES];
    uint8_t salt[AIRPLAY_SRP6A_SALT_BYTES];
    uint8_t public_a[AIRPLAY_SRP6A_PUBLIC_BYTES];
    uint8_t proof_m1[AIRPLAY_SRP6A_PROOF_BYTES];
    uint8_t expected_m2[AIRPLAY_SRP6A_PROOF_BYTES];
    uint8_t expected_key[AIRPLAY_SRP6A_SESSION_KEY_BYTES];
    uint8_t proof_m2[AIRPLAY_SRP6A_PROOF_BYTES];
    uint8_t response_state = 0U;
    size_t request_len = 0U;
    size_t response_len = 0U;
    const uint8_t state1 = 1U;
    const airplay_tlv8_item_t m1 = {
        .type = 0x06U, .value = &state1, .value_len = 1U,
    };
    request_len = encode_tlv(&m1, 1U, request, sizeof(request));
    assert(airplay_pair_setup_handle(session, request, request_len, response,
                                     sizeof(response), &response_len) ==
           AIRPLAY_PAIR_SETUP_OK);
    assert(get_tlv(response, response_len, 0x06U, &response_state, 1U) == 1U &&
           response_state == 2U);
    assert(get_tlv(response, response_len, 0x02U, salt, sizeof(salt)) ==
           sizeof(salt));
    assert(get_tlv(response, response_len, 0x03U, public_b,
                   sizeof(public_b)) == sizeof(public_b));

    airplay_srp6a_test_compute_client_proofs(
        salt, public_b, username, TEST_CODE, public_a, proof_m1, expected_m2,
        expected_key);
    if (corrupt_srp_proof) proof_m1[0] ^= 0x80U;
    const uint8_t state3 = 3U;
    const airplay_tlv8_item_t m3_items[] = {
        {.type = 0x06U, .value = &state3, .value_len = 1U},
        {.type = 0x03U, .value = public_a, .value_len = sizeof(public_a)},
        {.type = 0x04U, .value = proof_m1, .value_len = sizeof(proof_m1)},
    };
    request_len = encode_tlv(m3_items, 3U, request, sizeof(request));
    airplay_pair_setup_result_t result = airplay_pair_setup_handle(
        session, request, request_len, response, sizeof(response),
        &response_len);
    if (corrupt_srp_proof) {
        assert(result == AIRPLAY_PAIR_SETUP_AUTH_FAILED);
        assert(!airplay_pair_setup_is_complete(session));
        assert(fixture.save_calls == 0U);
        airplay_pair_setup_destroy(session);
        mbedtls_platform_zeroize(&fixture, sizeof(fixture));
        return;
    }
    assert(result == AIRPLAY_PAIR_SETUP_OK);
    response_state = 0U;
    assert(get_tlv(response, response_len, 0x06U, &response_state, 1U) == 1U &&
           response_state == 4U);
    assert(get_tlv(response, response_len, 0x04U, proof_m2,
                   sizeof(proof_m2)) == sizeof(proof_m2));
    assert(memcmp(proof_m2, expected_m2, sizeof(proof_m2)) == 0);

    uint8_t encryption_key[TEST_KEY_BYTES];
    uint8_t controller_sign_key[TEST_KEY_BYTES];
    uint8_t controller_signature_input[TEST_KEY_BYTES + sizeof(TEST_ID) - 1U +
                                       TEST_KEY_BYTES];
    uint8_t controller_signature[TEST_SIG_BYTES];
    derive_key(expected_key, sizeof(expected_key), "Pair-Setup-Encrypt-Salt",
               "Pair-Setup-Encrypt-Info", encryption_key);
    derive_key(expected_key, sizeof(expected_key),
               "Pair-Setup-Controller-Sign-Salt",
               "Pair-Setup-Controller-Sign-Info", controller_sign_key);
    memcpy(controller_signature_input, controller_sign_key,
           sizeof(controller_sign_key));
    memcpy(controller_signature_input + sizeof(controller_sign_key), TEST_ID,
           sizeof(TEST_ID) - 1U);
    memcpy(controller_signature_input + sizeof(controller_sign_key) +
               sizeof(TEST_ID) - 1U,
           fixture.controller_key, sizeof(fixture.controller_key));
    assert(fake_signature(fixture.controller_key, controller_signature_input,
                          sizeof(controller_signature_input),
                          controller_signature));
    if (corrupt_controller_signature) controller_signature[0] ^= 1U;
    const airplay_tlv8_item_t controller_items[] = {
        {.type = 0x01U, .value = (const uint8_t *)TEST_ID,
         .value_len = sizeof(TEST_ID) - 1U},
        {.type = 0x03U, .value = fixture.controller_key,
         .value_len = sizeof(fixture.controller_key)},
        {.type = 0x0aU, .value = controller_signature,
         .value_len = sizeof(controller_signature)},
    };
    uint8_t plaintext[TEST_PLAINTEXT_MAX];
    size_t plaintext_len = encode_tlv(controller_items, 3U, plaintext,
                                      sizeof(plaintext));
    uint8_t encrypted[TEST_PLAINTEXT_MAX + 16U];
    size_t encrypted_len = 0U;
    encrypt_message(encryption_key, "PS-Msg05", plaintext, plaintext_len,
                    encrypted, &encrypted_len);
    const uint8_t state5 = 5U;
    const airplay_tlv8_item_t m5_items[] = {
        {.type = 0x06U, .value = &state5, .value_len = 1U},
        {.type = 0x05U, .value = encrypted, .value_len = encrypted_len},
    };
    request_len = encode_tlv(m5_items, 2U, request, sizeof(request));
    result = airplay_pair_setup_handle(session, request, request_len, response,
                                       sizeof(response), &response_len);
    if (corrupt_controller_signature) {
        assert(result == AIRPLAY_PAIR_SETUP_AUTH_FAILED);
        assert(fixture.save_calls == 0U);
        assert(!airplay_pair_setup_is_complete(session));
    } else if (fail_storage) {
        assert(result == AIRPLAY_PAIR_SETUP_STORAGE_FAILED);
        assert(fixture.save_calls == 1U);
        assert(!airplay_pair_setup_is_complete(session));
    } else {
        assert(result == AIRPLAY_PAIR_SETUP_OK);
        assert(airplay_pair_setup_is_complete(session));
        assert(fixture.save_calls == 1U);
        assert(fixture.saved_identifier_len == sizeof(TEST_ID) - 1U);
        assert(memcmp(fixture.saved_identifier, TEST_ID,
                      sizeof(TEST_ID) - 1U) == 0);
        assert(memcmp(fixture.saved_public_key, fixture.controller_key,
                      sizeof(fixture.controller_key)) == 0);
        response_state = 0U;
        assert(get_tlv(response, response_len, 0x06U, &response_state, 1U) == 1U &&
               response_state == 6U);
        uint8_t response_encrypted[TEST_PLAINTEXT_MAX];
        size_t response_encrypted_len = get_tlv(
            response, response_len, 0x05U, response_encrypted,
            sizeof(response_encrypted));
        uint8_t response_plaintext[TEST_PLAINTEXT_MAX];
        size_t response_plaintext_len = decrypt_message(
            encryption_key, "PS-Msg06", response_encrypted,
            response_encrypted_len, response_plaintext,
            sizeof(response_plaintext));
        uint8_t accessory_id[AIRPLAY_PAIR_SETUP_MAX_IDENTIFIER_BYTES];
        uint8_t accessory_key[TEST_KEY_BYTES];
        uint8_t accessory_signature[TEST_SIG_BYTES];
        size_t accessory_id_len = get_tlv(
            response_plaintext, response_plaintext_len, 0x01U, accessory_id,
            sizeof(accessory_id));
        assert(accessory_id_len == sizeof(TEST_ACCESSORY_ID) - 1U);
        assert(memcmp(accessory_id, TEST_ACCESSORY_ID, accessory_id_len) == 0);
        assert(get_tlv(response_plaintext, response_plaintext_len, 0x03U,
                       accessory_key, sizeof(accessory_key)) ==
               sizeof(accessory_key));
        assert(get_tlv(response_plaintext, response_plaintext_len, 0x0aU,
                       accessory_signature, sizeof(accessory_signature)) ==
               sizeof(accessory_signature));
        uint8_t accessory_sign_key[TEST_KEY_BYTES];
        uint8_t accessory_signature_input[TEST_KEY_BYTES +
                                          sizeof(TEST_ACCESSORY_ID) - 1U +
                                          TEST_KEY_BYTES];
        uint8_t expected_signature[TEST_SIG_BYTES];
        derive_key(expected_key, sizeof(expected_key),
                   "Pair-Setup-Accessory-Sign-Salt",
                   "Pair-Setup-Accessory-Sign-Info", accessory_sign_key);
        memcpy(accessory_signature_input, accessory_sign_key,
               sizeof(accessory_sign_key));
        memcpy(accessory_signature_input + sizeof(accessory_sign_key),
               TEST_ACCESSORY_ID, sizeof(TEST_ACCESSORY_ID) - 1U);
        memcpy(accessory_signature_input + sizeof(accessory_sign_key) +
                   sizeof(TEST_ACCESSORY_ID) - 1U,
               accessory_key, sizeof(accessory_key));
        assert(fake_signature(fixture.accessory_secret,
                              accessory_signature_input,
                              sizeof(accessory_signature_input),
                              expected_signature));
        assert(memcmp(expected_signature, accessory_signature,
                      sizeof(expected_signature)) == 0);
        mbedtls_platform_zeroize(response_encrypted,
                                 sizeof(response_encrypted));
        mbedtls_platform_zeroize(response_plaintext,
                                 sizeof(response_plaintext));
        mbedtls_platform_zeroize(accessory_sign_key,
                                 sizeof(accessory_sign_key));
        mbedtls_platform_zeroize(accessory_signature_input,
                                 sizeof(accessory_signature_input));
        mbedtls_platform_zeroize(expected_signature,
                                 sizeof(expected_signature));
    }
    airplay_pair_setup_destroy(session);
    mbedtls_platform_zeroize(&fixture, sizeof(fixture));
    mbedtls_platform_zeroize(expected_key, sizeof(expected_key));
    mbedtls_platform_zeroize(encryption_key, sizeof(encryption_key));
}

static void test_random_failure(void)
{
    provider_fixture_t fixture = {.fail_random = true};
    airplay_pair_setup_config_t config;
    setup_config(&fixture, &config);
    airplay_pair_setup_t *session = NULL;
    assert(airplay_pair_setup_create(&config, &session) == AIRPLAY_PAIR_SETUP_OK);
    const uint8_t state = 1U;
    const airplay_tlv8_item_t item = {
        .type = 0x06U, .value = &state, .value_len = 1U,
    };
    uint8_t request[16];
    uint8_t response[16];
    size_t request_len = encode_tlv(&item, 1U, request, sizeof(request));
    size_t response_len = 0U;
    assert(airplay_pair_setup_handle(session, request, request_len, response,
                                     sizeof(response), &response_len) ==
           AIRPLAY_PAIR_SETUP_RANDOM_FAILED);
    assert(response_len != 0U);
    assert(!airplay_pair_setup_is_complete(session));
    airplay_pair_setup_destroy(session);
    mbedtls_platform_zeroize(&fixture, sizeof(fixture));
}

int main(void)
{
    run_exchange(false, false, false);
    run_exchange(true, false, false);
    run_exchange(false, true, false);
    run_exchange(false, false, true);
    test_random_failure();
    puts("airplay Pair-Setup host test passed (test-only signature provider shim)");
    return 0;
}
