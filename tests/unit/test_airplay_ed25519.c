/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_ed25519.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void decode_hex(const char *hex, uint8_t *output, size_t output_len)
{
    for (size_t i = 0U; i < output_len; ++i) {
        unsigned value = 0U;
        assert(sscanf(hex + i * 2U, "%2x", &value) == 1);
        output[i] = (uint8_t)value;
    }
}

static void test_rfc8032_vector(const char *seed_hex,
                                const char *public_hex,
                                const char *message_hex,
                                const char *signature_hex)
{
    uint8_t seed[AIRPLAY_ED25519_SEED_BYTES];
    uint8_t expected_public[AIRPLAY_ED25519_PUBLIC_KEY_BYTES];
    uint8_t expected_signature[AIRPLAY_ED25519_SIGNATURE_BYTES];
    uint8_t message[64];
    size_t message_len = strlen(message_hex) / 2U;
    decode_hex(seed_hex, seed, sizeof(seed));
    decode_hex(public_hex, expected_public, sizeof(expected_public));
    decode_hex(signature_hex, expected_signature, sizeof(expected_signature));
    if (message_len != 0U) decode_hex(message_hex, message, message_len);

    airplay_ed25519_keypair_t keypair;
    assert(airplay_ed25519_keypair_from_seed(seed, &keypair));
    assert(memcmp(keypair.public_key, expected_public,
                  sizeof(expected_public)) == 0);

    uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES];
    assert(airplay_ed25519_sign(&keypair, message, message_len, signature));
    assert(memcmp(signature, expected_signature, sizeof(expected_signature)) ==
           0);
    assert(airplay_ed25519_verify(expected_public, message, message_len,
                                  expected_signature));

    /* Any single-bit change in message, signature or key must fail. */
    if (message_len != 0U) {
        message[0] ^= 0x01U;
        assert(!airplay_ed25519_verify(expected_public, message, message_len,
                                       expected_signature));
        message[0] ^= 0x01U;
    }
    signature[0] ^= 0x80U;
    assert(!airplay_ed25519_verify(expected_public, message, message_len,
                                   signature));
    signature[0] ^= 0x80U;
    expected_public[0] ^= 0x01U;
    assert(!airplay_ed25519_verify(expected_public, message, message_len,
                                   expected_signature));

    airplay_ed25519_keypair_clear(&keypair);
    for (size_t i = 0U; i < sizeof(keypair.private_key); ++i) {
        assert(keypair.private_key[i] == 0U);
    }
}

static void test_argument_checks(void)
{
    uint8_t seed[AIRPLAY_ED25519_SEED_BYTES] = {0};
    airplay_ed25519_keypair_t keypair;
    uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES];
    assert(!airplay_ed25519_keypair_from_seed(NULL, &keypair));
    assert(!airplay_ed25519_keypair_from_seed(seed, NULL));
    assert(airplay_ed25519_keypair_from_seed(seed, &keypair));
    assert(!airplay_ed25519_sign(NULL, seed, sizeof(seed), signature));
    assert(!airplay_ed25519_sign(&keypair, NULL, 1U, signature));
    assert(airplay_ed25519_sign(&keypair, NULL, 0U, signature));
    assert(airplay_ed25519_verify(keypair.public_key, NULL, 0U, signature));
    assert(!airplay_ed25519_verify(NULL, seed, sizeof(seed), signature));
    assert(!airplay_ed25519_verify(keypair.public_key, seed, 1U, signature));
    assert(!airplay_ed25519_verify(keypair.public_key, seed, sizeof(seed),
                                   signature));
    airplay_ed25519_keypair_clear(NULL);
}

int main(void)
{
    test_rfc8032_vector(
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    test_rfc8032_vector(
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    test_rfc8032_vector(
        "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
        "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
        "af82",
        "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
        "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a");
    test_argument_checks();
    puts("airplay Ed25519 host test passed");
    return 0;
}
