/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Thin wrapper over the vendored orlp/ed25519 implementation
 * (third_party/ed25519, zlib license).  It is the software identity provider
 * for Pair-Setup/Pair-Verify: the caller supplies the seed, and the private
 * key never has to leave this structure except when signing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_ED25519_SEED_BYTES 32U
#define AIRPLAY_ED25519_PUBLIC_KEY_BYTES 32U
#define AIRPLAY_ED25519_SIGNATURE_BYTES 64U

typedef struct {
    uint8_t public_key[AIRPLAY_ED25519_PUBLIC_KEY_BYTES];
    /* seed || public key, the layout orlp/ed25519 expects when signing. */
    uint8_t private_key[64];
} airplay_ed25519_keypair_t;

/* Deterministically expands a 32-byte seed into the key pair. */
bool airplay_ed25519_keypair_from_seed(
    const uint8_t seed[AIRPLAY_ED25519_SEED_BYTES],
    airplay_ed25519_keypair_t *out_keypair);

bool airplay_ed25519_sign(const airplay_ed25519_keypair_t *keypair,
                          const uint8_t *message,
                          size_t message_len,
                          uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES]);

bool airplay_ed25519_verify(
    const uint8_t public_key[AIRPLAY_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t *message,
    size_t message_len,
    const uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES]);

/* Zeroizes the private half; the public key stays readable. */
void airplay_ed25519_keypair_clear(airplay_ed25519_keypair_t *keypair);
