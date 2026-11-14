/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>

#include "airplay_srp6a.h"

void airplay_srp6a_test_compute_client_proofs(
    const uint8_t salt[AIRPLAY_SRP6A_SALT_BYTES],
    const uint8_t public_b[AIRPLAY_SRP6A_PUBLIC_BYTES],
    const char *username,
    const char *password,
    uint8_t public_a[AIRPLAY_SRP6A_PUBLIC_BYTES],
    uint8_t proof_m1[AIRPLAY_SRP6A_PROOF_BYTES],
    uint8_t expected_m2[AIRPLAY_SRP6A_PROOF_BYTES],
    uint8_t expected_key[AIRPLAY_SRP6A_SESSION_KEY_BYTES]);
