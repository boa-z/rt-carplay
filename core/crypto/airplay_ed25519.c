/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_ed25519.h"

#include <string.h>

#include "ed25519.h"
#include "mbedtls/platform_util.h"

bool airplay_ed25519_keypair_from_seed(
    const uint8_t seed[AIRPLAY_ED25519_SEED_BYTES],
    airplay_ed25519_keypair_t *out_keypair)
{
    if (seed == NULL || out_keypair == NULL) return false;
    ed25519_create_keypair(out_keypair->public_key, out_keypair->private_key,
                           seed);
    return true;
}

bool airplay_ed25519_sign(const airplay_ed25519_keypair_t *keypair,
                          const uint8_t *message,
                          size_t message_len,
                          uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES])
{
    if (keypair == NULL || signature == NULL ||
        (message == NULL && message_len != 0U)) {
        return false;
    }
    ed25519_sign(signature, message, message_len, keypair->public_key,
                 keypair->private_key);
    return true;
}

bool airplay_ed25519_verify(
    const uint8_t public_key[AIRPLAY_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t *message,
    size_t message_len,
    const uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES])
{
    if (public_key == NULL || signature == NULL ||
        (message == NULL && message_len != 0U)) {
        return false;
    }
    return ed25519_verify(signature, message, message_len, public_key) != 0;
}

void airplay_ed25519_keypair_clear(airplay_ed25519_keypair_t *keypair)
{
    if (keypair == NULL) return;
    mbedtls_platform_zeroize(keypair->private_key,
                             sizeof(keypair->private_key));
}
