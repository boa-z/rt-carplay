/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_pair_verify.h"
#include "airplay_tlv8.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/chachapoly.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

#define TEST_KEY_BYTES 32U
#define TEST_SIG_BYTES 64U
#define TEST_NONCE_BYTES 12U
#define TEST_ID "controller-test"
#define TEST_ACCESSORY_ID "accessory-test"

typedef struct {
    uint8_t random_counter;
    bool fail_random;
    const uint8_t *controller_key;
    bool fail_verify;
    uint8_t accessory_secret[TEST_KEY_BYTES];
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

static int test_rng(void *context, unsigned char *output, size_t output_len)
{
    uint8_t *counter = context;
    for (size_t i = 0; i < output_len; ++i) output[i] = (*counter)++;
    return 0;
}

/* Test-only signature shim. It exercises the provider boundary and message
 * layout, but deliberately does not implement or claim Ed25519. */
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
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(expected); ++i) diff |= expected[i] ^ signature[i];
    mbedtls_platform_zeroize(expected, sizeof(expected));
    return diff == 0;
}

static bool lookup_controller(const uint8_t *identifier,
                              size_t identifier_len,
                              uint8_t public_key[TEST_KEY_BYTES],
                              void *context)
{
    provider_fixture_t *fixture = context;
    if (identifier_len != sizeof(TEST_ID) - 1U ||
        memcmp(identifier, TEST_ID, sizeof(TEST_ID) - 1U) != 0) {
        return false;
    }
    memcpy(public_key, fixture->controller_key, TEST_KEY_BYTES);
    return true;
}

static void nonce_for(const char label[8], uint8_t nonce[TEST_NONCE_BYTES])
{
    memset(nonce, 0, TEST_NONCE_BYTES);
    memcpy(nonce + 4U, label, 8U);
}

static size_t encode_tlv(const airplay_tlv8_item_t *items,
                         size_t item_count,
                         uint8_t *output,
                         size_t capacity)
{
    size_t written = 0;
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
    size_t written = 0;
    assert(airplay_tlv8_get(input, input_len, type, output, capacity,
                            &written) == AIRPLAY_TLV8_OK);
    return written;
}

static void client_encrypt(const uint8_t key[TEST_KEY_BYTES],
                           const char nonce_label[8],
                           const uint8_t *plaintext,
                           size_t plaintext_len,
                           uint8_t *output,
                           size_t *output_len)
{
    uint8_t nonce[TEST_NONCE_BYTES];
    nonce_for(nonce_label, nonce);
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

static size_t client_decrypt(const uint8_t key[TEST_KEY_BYTES],
                             const char nonce_label[8],
                             const uint8_t *input,
                             size_t input_len,
                             uint8_t *plaintext,
                             size_t plaintext_capacity)
{
    assert(input_len >= 16U && input_len - 16U <= plaintext_capacity);
    uint8_t nonce[TEST_NONCE_BYTES];
    nonce_for(nonce_label, nonce);
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

static void derive_key(const uint8_t shared[TEST_KEY_BYTES],
                       const char *salt,
                       const char *info,
                       uint8_t output[TEST_KEY_BYTES])
{
    const mbedtls_md_info_t *sha512 =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    assert(sha512 != NULL);
    assert(mbedtls_hkdf(sha512, (const uint8_t *)salt, strlen(salt),
                        shared, TEST_KEY_BYTES, (const uint8_t *)info,
                        strlen(info), output, TEST_KEY_BYTES) == 0);
}

static void decode_hex(const char *hex, uint8_t *output, size_t output_len)
{
    assert(strlen(hex) == output_len * 2U);
    for (size_t i = 0; i < output_len; ++i) {
        unsigned int value = 0U;
        assert(sscanf(hex + i * 2U, "%2x", &value) == 1);
        output[i] = (uint8_t)value;
    }
}

static void test_x25519_rfc7748_vector(void)
{
    static const char alice_private_hex[] =
        "77076d0a7318a57d3c16c17251b26645"
        "df4c2f87ebc0992ab177fba51db92c2a";
    static const char bob_public_hex[] =
        "de9edb7d7b7dc1b4d35b61c2ece43537"
        "3f8343c85b78674dadfc7e146f882b4f";
    static const char shared_hex[] =
        "4a5d9d5ba4ce2de1728e3bf480350f25"
        "e07e21c947d19e3376f09b3c1e161742";
    uint8_t alice_private[TEST_KEY_BYTES];
    uint8_t bob_public[TEST_KEY_BYTES];
    uint8_t expected_shared[TEST_KEY_BYTES];
    uint8_t shared[TEST_KEY_BYTES];
    decode_hex(alice_private_hex, alice_private, sizeof(alice_private));
    decode_hex(bob_public_hex, bob_public, sizeof(bob_public));
    decode_hex(shared_hex, expected_shared, sizeof(expected_shared));

    mbedtls_ecp_group group;
    mbedtls_ecp_point peer;
    mbedtls_mpi private_key;
    mbedtls_mpi shared_mpi;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&peer);
    mbedtls_mpi_init(&private_key);
    mbedtls_mpi_init(&shared_mpi);
    assert(mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_CURVE25519) == 0);
    assert(mbedtls_mpi_read_binary_le(&private_key, alice_private,
                                      sizeof(alice_private)) == 0);
    assert(mbedtls_mpi_set_bit(&private_key, 0, 0) == 0);
    assert(mbedtls_mpi_set_bit(&private_key, 1, 0) == 0);
    assert(mbedtls_mpi_set_bit(&private_key, 2, 0) == 0);
    assert(mbedtls_mpi_set_bit(&private_key, 255, 0) == 0);
    assert(mbedtls_mpi_set_bit(&private_key, 254, 1) == 0);
    assert(mbedtls_ecp_point_read_binary(&group, &peer, bob_public,
                                         sizeof(bob_public)) == 0);
    uint8_t rng_counter = 0U;
    assert(mbedtls_ecdh_compute_shared(&group, &shared_mpi, &peer,
                                       &private_key, test_rng,
                                       &rng_counter) == 0);
    assert(mbedtls_mpi_write_binary_le(&shared_mpi, shared,
                                       sizeof(shared)) == 0);
    assert(memcmp(shared, expected_shared, sizeof(shared)) == 0);
    mbedtls_ecp_group_free(&group);
    mbedtls_ecp_point_free(&peer);
    mbedtls_mpi_free(&private_key);
    mbedtls_mpi_free(&shared_mpi);
}

static void test_complete_exchange(bool tamper_controller_signature)
{
    uint8_t controller_private_counter = 91U;
    uint8_t server_rng_counter = 23U;
    uint8_t controller_public[TEST_KEY_BYTES];
    uint8_t accessory_public[TEST_KEY_BYTES];
    uint8_t shared[TEST_KEY_BYTES];
    uint8_t encryption_key[TEST_KEY_BYTES];
    uint8_t response[AIRPLAY_PAIR_VERIFY_MAX_MESSAGE_BYTES];
    uint8_t request[AIRPLAY_PAIR_VERIFY_MAX_MESSAGE_BYTES];
    uint8_t encrypted[512];
    uint8_t submessage[512];
    size_t request_len = 0U;
    size_t response_len = 0U;
    size_t encrypted_len = 0U;
    size_t submessage_len = 0U;
    mbedtls_ecp_group client_group;
    mbedtls_ecp_point client_public_point;
    mbedtls_ecp_point accessory_public_point;
    mbedtls_ecp_point shared_point;
    mbedtls_mpi client_private;
    mbedtls_mpi shared_mpi;
    mbedtls_ecp_group_init(&client_group);
    mbedtls_ecp_point_init(&client_public_point);
    mbedtls_ecp_point_init(&accessory_public_point);
    mbedtls_ecp_point_init(&shared_point);
    mbedtls_mpi_init(&client_private);
    mbedtls_mpi_init(&shared_mpi);
    assert(mbedtls_ecp_group_load(&client_group,
                                  MBEDTLS_ECP_DP_CURVE25519) == 0);
    assert(mbedtls_ecp_gen_keypair(&client_group, &client_private,
                                   &client_public_point, test_rng,
                                   &controller_private_counter) == 0);
    size_t public_len = 0U;
    assert(mbedtls_ecp_point_write_binary(
               &client_group, &client_public_point,
               MBEDTLS_ECP_PF_UNCOMPRESSED, &public_len, controller_public,
               sizeof(controller_public)) == 0);
    assert(public_len == sizeof(controller_public));

    provider_fixture_t fixture = {0};
    fixture.random_counter = 11U;
    fixture.controller_key = (const uint8_t[TEST_KEY_BYTES]){
        0x43,0x4f,0x4e,0x54,0x52,0x4f,0x4c,0x4c,
        0x45,0x52,0x2d,0x54,0x45,0x53,0x54,0x2d,
        0x4b,0x45,0x59,0x2d,0x4d,0x41,0x54,0x45,
        0x52,0x49,0x41,0x4c,0x2d,0x30,0x30,0x31,
    };
    for (size_t i = 0; i < sizeof(fixture.accessory_secret); ++i) {
        fixture.accessory_secret[i] = (uint8_t)(0xa0U + i);
    }
    airplay_pair_verify_config_t config = {
        .accessory_identifier = (const uint8_t *)TEST_ACCESSORY_ID,
        .accessory_identifier_len = sizeof(TEST_ACCESSORY_ID) - 1U,
        .random = test_random,
        .sign_accessory = sign_accessory,
        .verify_controller = verify_controller,
        .lookup_controller = lookup_controller,
        .provider_context = &fixture,
    };
    airplay_pair_verify_t *session = NULL;
    assert(airplay_pair_verify_create(&config, &session) ==
           AIRPLAY_PAIR_VERIFY_OK);
    const uint8_t state1 = 1U;
    const airplay_tlv8_item_t m1_items[] = {
        {.type = 0x06U, .value = &state1, .value_len = 1U},
        {.type = 0x03U, .value = controller_public,
         .value_len = sizeof(controller_public)},
    };
    request_len = encode_tlv(m1_items, 2U, request, sizeof(request));
    assert(airplay_pair_verify_handle(session, request, request_len, response,
                                      sizeof(response), &response_len) ==
           AIRPLAY_PAIR_VERIFY_OK);
    uint8_t response_state = 0U;
    assert(get_tlv(response, response_len, 0x06U, &response_state, 1U) == 1U &&
           response_state == 2U);
    assert(get_tlv(response, response_len, 0x03U, accessory_public,
                   sizeof(accessory_public)) == sizeof(accessory_public));
    encrypted_len = get_tlv(response, response_len, 0x05U, encrypted,
                            sizeof(encrypted));

    assert(mbedtls_ecp_point_read_binary(&client_group,
                                         &accessory_public_point,
                                         accessory_public,
                                         sizeof(accessory_public)) == 0);
    assert(mbedtls_ecdh_compute_shared(&client_group, &shared_mpi,
                                       &accessory_public_point,
                                       &client_private, test_rng,
                                       &server_rng_counter) == 0);
    assert(mbedtls_mpi_write_binary_le(&shared_mpi, shared,
                                       sizeof(shared)) == 0);
    derive_key(shared, "Pair-Verify-Encrypt-Salt",
               "Pair-Verify-Encrypt-Info", encryption_key);
    submessage_len = client_decrypt(encryption_key, "PV-Msg02", encrypted,
                                    encrypted_len, submessage,
                                    sizeof(submessage));
    uint8_t accessory_id[64];
    uint8_t accessory_signature[TEST_SIG_BYTES];
    size_t accessory_id_len = get_tlv(submessage, submessage_len, 0x01U,
                                      accessory_id, sizeof(accessory_id));
    assert(accessory_id_len == sizeof(TEST_ACCESSORY_ID) - 1U);
    assert(memcmp(accessory_id, TEST_ACCESSORY_ID, accessory_id_len) == 0);
    assert(get_tlv(submessage, submessage_len, 0x0aU, accessory_signature,
                   sizeof(accessory_signature)) ==
           sizeof(accessory_signature));
    uint8_t accessory_signature_input[128];
    memcpy(accessory_signature_input, accessory_public, TEST_KEY_BYTES);
    memcpy(accessory_signature_input + TEST_KEY_BYTES, TEST_ACCESSORY_ID,
           sizeof(TEST_ACCESSORY_ID) - 1U);
    memcpy(accessory_signature_input + TEST_KEY_BYTES +
               sizeof(TEST_ACCESSORY_ID) - 1U,
           controller_public, TEST_KEY_BYTES);
    uint8_t expected_signature[TEST_SIG_BYTES];
    assert(fake_signature(fixture.accessory_secret,
                          accessory_signature_input,
                          TEST_KEY_BYTES + sizeof(TEST_ACCESSORY_ID) - 1U +
                              TEST_KEY_BYTES,
                          expected_signature));
    assert(memcmp(expected_signature, accessory_signature,
                  sizeof(expected_signature)) == 0);

    uint8_t controller_signature_input[128];
    memcpy(controller_signature_input, controller_public, TEST_KEY_BYTES);
    memcpy(controller_signature_input + TEST_KEY_BYTES, TEST_ID,
           sizeof(TEST_ID) - 1U);
    memcpy(controller_signature_input + TEST_KEY_BYTES + sizeof(TEST_ID) - 1U,
           accessory_public, TEST_KEY_BYTES);
    uint8_t controller_signature[TEST_SIG_BYTES];
    assert(fake_signature(fixture.controller_key, controller_signature_input,
                          TEST_KEY_BYTES + sizeof(TEST_ID) - 1U + TEST_KEY_BYTES,
                          controller_signature));
    if (tamper_controller_signature) controller_signature[0] ^= 0x01U;
    const airplay_tlv8_item_t m3_sub_items[] = {
        {.type = 0x01U, .value = (const uint8_t *)TEST_ID,
         .value_len = sizeof(TEST_ID) - 1U},
        {.type = 0x0aU, .value = controller_signature,
         .value_len = sizeof(controller_signature)},
    };
    submessage_len = encode_tlv(m3_sub_items, 2U, submessage,
                                sizeof(submessage));
    client_encrypt(encryption_key, "PV-Msg03", submessage, submessage_len,
                   encrypted, &encrypted_len);
    const uint8_t state3 = 3U;
    const airplay_tlv8_item_t m3_items[] = {
        {.type = 0x06U, .value = &state3, .value_len = 1U},
        {.type = 0x05U, .value = encrypted, .value_len = encrypted_len},
    };
    request_len = encode_tlv(m3_items, 2U, request, sizeof(request));
    airplay_pair_verify_result_t result = airplay_pair_verify_handle(
        session, request, request_len, response, sizeof(response),
        &response_len);
    if (tamper_controller_signature) {
        assert(result == AIRPLAY_PAIR_VERIFY_AUTH_FAILED);
        airplay_pair_verify_control_keys_t keys;
        assert(airplay_pair_verify_get_control_keys(session, &keys) ==
               AIRPLAY_PAIR_VERIFY_STATE_ERROR);
        airplay_pair_verify_destroy(session);
    } else {
        assert(result == AIRPLAY_PAIR_VERIFY_OK);
        response_state = 0U;
        assert(get_tlv(response, response_len, 0x06U, &response_state, 1U) ==
               1U);
        assert(response_state == 4U);
        airplay_pair_verify_control_keys_t keys;
        assert(airplay_pair_verify_get_control_keys(session, &keys) ==
               AIRPLAY_PAIR_VERIFY_OK);
        uint8_t expected_read[TEST_KEY_BYTES];
        uint8_t expected_write[TEST_KEY_BYTES];
        derive_key(shared, "Control-Salt", "Control-Write-Encryption-Key",
                   expected_read);
        derive_key(shared, "Control-Salt", "Control-Read-Encryption-Key",
                   expected_write);
        assert(memcmp(keys.read_key, expected_read, sizeof(expected_read)) == 0);
        assert(memcmp(keys.write_key, expected_write,
                      sizeof(expected_write)) == 0);
        airplay_pair_verify_event_keys_t event_keys;
        assert(airplay_pair_verify_get_event_keys(session, &event_keys) ==
               AIRPLAY_PAIR_VERIFY_OK);
        uint8_t expected_event_read[TEST_KEY_BYTES];
        uint8_t expected_event_write[TEST_KEY_BYTES];
        derive_key(shared, "Events-Salt", "Events-Read-Encryption-Key",
                   expected_event_read);
        derive_key(shared, "Events-Salt", "Events-Write-Encryption-Key",
                   expected_event_write);
        assert(memcmp(event_keys.read_key, expected_event_read,
                      sizeof(expected_event_read)) == 0);
        assert(memcmp(event_keys.write_key, expected_event_write,
                      sizeof(expected_event_write)) == 0);
        const uint64_t stream_connection_id = UINT64_C(72623859790382856);
        char stream_salt[48];
        int stream_salt_len = snprintf(stream_salt, sizeof(stream_salt),
                                       "DataStream-Salt%" PRIu64,
                                       stream_connection_id);
        assert(stream_salt_len > 0 && (size_t)stream_salt_len < sizeof(stream_salt));
        uint8_t expected_screen_key[TEST_KEY_BYTES];
        uint8_t screen_key[TEST_KEY_BYTES];
        derive_key(shared, stream_salt, "DataStream-Output-Encryption-Key",
                   expected_screen_key);
        assert(airplay_pair_verify_derive_screen_output_key(
                   session, stream_connection_id, screen_key) ==
               AIRPLAY_PAIR_VERIFY_OK);
        assert(memcmp(screen_key, expected_screen_key,
                      sizeof(expected_screen_key)) == 0);
        airplay_pair_verify_destroy(session);
    }
    mbedtls_ecp_group_free(&client_group);
    mbedtls_ecp_point_free(&client_public_point);
    mbedtls_ecp_point_free(&accessory_public_point);
    mbedtls_ecp_point_free(&shared_point);
    mbedtls_mpi_free(&client_private);
    mbedtls_mpi_free(&shared_mpi);
    mbedtls_platform_zeroize(shared, sizeof(shared));
    mbedtls_platform_zeroize(encryption_key, sizeof(encryption_key));
    mbedtls_platform_zeroize(&fixture, sizeof(fixture));
}

int main(void)
{
    test_x25519_rfc7748_vector();
    test_complete_exchange(false);
    test_complete_exchange(true);

    uint8_t output[128];
    size_t output_len = 0U;
    uint8_t counter = 0U;
    provider_fixture_t fixture = {.random_counter = 0U, .fail_random = true};
    uint8_t controller_key[TEST_KEY_BYTES] = {0};
    fixture.controller_key = controller_key;
    airplay_pair_verify_config_t config = {
        .accessory_identifier = (const uint8_t *)TEST_ACCESSORY_ID,
        .accessory_identifier_len = sizeof(TEST_ACCESSORY_ID) - 1U,
        .random = test_random,
        .sign_accessory = sign_accessory,
        .verify_controller = verify_controller,
        .lookup_controller = lookup_controller,
        .provider_context = &fixture,
    };
    airplay_pair_verify_t *session = NULL;
    assert(airplay_pair_verify_create(&config, &session) ==
           AIRPLAY_PAIR_VERIFY_OK);
    const uint8_t state1 = 1U;
    const uint8_t public_key[TEST_KEY_BYTES] = {9U};
    const airplay_tlv8_item_t request_items[] = {
        {.type = 0x06U, .value = &state1, .value_len = 1U},
        {.type = 0x03U, .value = public_key, .value_len = sizeof(public_key)},
    };
    size_t request_len = encode_tlv(request_items, 2U, output, sizeof(output));
    uint8_t response[128];
    assert(airplay_pair_verify_handle(session, output, request_len, response,
                                      sizeof(response), &output_len) ==
           AIRPLAY_PAIR_VERIFY_RANDOM_FAILED);
    airplay_pair_verify_destroy(session);
    (void)counter;
    puts("airplay Pair-Verify host test passed (Ed25519 is a test provider shim)");
    return 0;
}
