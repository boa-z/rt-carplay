/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SRP-6a server API matching DiPlay's AirPlay Pair-Setup variant. See
 * THIRD_PARTY_NOTICES.md. The caller supplies the pairing code and CSPRNG.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_SRP6A_SALT_BYTES 16U
#define AIRPLAY_SRP6A_PUBLIC_BYTES 384U
#define AIRPLAY_SRP6A_PROOF_BYTES 64U
#define AIRPLAY_SRP6A_SESSION_KEY_BYTES 64U

typedef struct airplay_srp6a_server airplay_srp6a_server_t;

typedef enum {
    AIRPLAY_SRP6A_OK = 0,
    AIRPLAY_SRP6A_INVALID_ARGUMENT,
    AIRPLAY_SRP6A_NO_MEMORY,
    AIRPLAY_SRP6A_RANDOM_FAILED,
    AIRPLAY_SRP6A_CRYPTO_FAILED,
    AIRPLAY_SRP6A_AUTH_FAILED,
    AIRPLAY_SRP6A_STATE_ERROR,
} airplay_srp6a_result_t;

typedef bool (*airplay_srp6a_random_cb_t)(uint8_t *output, size_t output_len, void *context);

/* Start the accessory-side Pair-Setup SRP exchange. No pairing code is
 * embedded in this component; pass the code from a provisioned configuration
 * source and a cryptographically secure random callback. */
airplay_srp6a_result_t airplay_srp6a_server_start(const char *username,
                                                  const char *pairing_code,
                                                  airplay_srp6a_random_cb_t random_cb,
                                                  void *random_context,
                                                  airplay_srp6a_server_t **out_server,
                                                  uint8_t out_salt[AIRPLAY_SRP6A_SALT_BYTES],
                                                  uint8_t out_public_b[AIRPLAY_SRP6A_PUBLIC_BYTES]);

/* Verify the controller's SRP proof. A context accepts at most one attempt.
 * On success, returns the 64-byte shared session key and server proof. */
airplay_srp6a_result_t airplay_srp6a_server_verify(airplay_srp6a_server_t *server,
                                                   const uint8_t *public_a,
                                                   size_t public_a_len,
                                                   const uint8_t *proof_m1,
                                                   size_t proof_m1_len,
                                                   uint8_t out_session_key[AIRPLAY_SRP6A_SESSION_KEY_BYTES],
                                                   uint8_t out_proof_m2[AIRPLAY_SRP6A_PROOF_BYTES]);

void airplay_srp6a_server_destroy(airplay_srp6a_server_t *server);
