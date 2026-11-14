/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_srp6a.h"
#include "airplay_srp6a_test_client.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/md.h"

#define GROUP_BYTES AIRPLAY_SRP6A_PUBLIC_BYTES


static bool deterministic_random(uint8_t *output, size_t output_len, void *context)
{
    uint8_t *counter = context;
    if (output == NULL || counter == NULL) return false;
    for (size_t i = 0; i < output_len; ++i) output[i] = (*counter)++;
    return true;
}

static bool failing_random(uint8_t *output, size_t output_len, void *context)
{
    (void)output;
    (void)output_len;
    (void)context;
    return false;
}


int main(void)
{
    static const char username[] = "Pair-Setup";
    static const char pairing_code[] = "test-only-code";
    uint8_t rng_counter = 0;
    uint8_t salt[AIRPLAY_SRP6A_SALT_BYTES], public_b[GROUP_BYTES];
    airplay_srp6a_server_t *server = NULL;
    assert(airplay_srp6a_server_start(username, pairing_code, deterministic_random,
                                      &rng_counter, &server, salt, public_b) == AIRPLAY_SRP6A_OK);
    assert(server != NULL);

    uint8_t public_a[GROUP_BYTES], proof_m1[64], expected_m2[64], expected_key[64];
    uint8_t session_key[64], proof_m2[64];
    airplay_srp6a_test_compute_client_proofs(salt, public_b, username, pairing_code,
                                             public_a, proof_m1, expected_m2, expected_key);
    assert(airplay_srp6a_server_verify(server, public_a, sizeof(public_a),
                                       proof_m1, sizeof(proof_m1),
                                       session_key, proof_m2) == AIRPLAY_SRP6A_OK);
    assert(memcmp(session_key, expected_key, sizeof(session_key)) == 0);
    assert(memcmp(proof_m2, expected_m2, sizeof(proof_m2)) == 0);
    assert(airplay_srp6a_server_verify(server, public_a, sizeof(public_a),
                                       proof_m1, sizeof(proof_m1),
                                       session_key, proof_m2) == AIRPLAY_SRP6A_STATE_ERROR);
    airplay_srp6a_server_destroy(server);

    rng_counter = 0;
    assert(airplay_srp6a_server_start(username, pairing_code, deterministic_random,
                                      &rng_counter, &server, salt, public_b) == AIRPLAY_SRP6A_OK);
    airplay_srp6a_test_compute_client_proofs(salt, public_b, username, pairing_code,
                                             public_a, proof_m1, expected_m2, expected_key);
    proof_m1[0] ^= 0x80U;
    memset(session_key, 0xa5, sizeof(session_key));
    memset(proof_m2, 0xa5, sizeof(proof_m2));
    assert(airplay_srp6a_server_verify(server, public_a, sizeof(public_a),
                                       proof_m1, sizeof(proof_m1),
                                       session_key, proof_m2) == AIRPLAY_SRP6A_AUTH_FAILED);
    uint8_t zero[64] = {0};
    assert(memcmp(session_key, zero, sizeof(session_key)) == 0);
    assert(memcmp(proof_m2, zero, sizeof(proof_m2)) == 0);
    assert(airplay_srp6a_server_verify(server, public_a, sizeof(public_a),
                                       proof_m1, sizeof(proof_m1),
                                       session_key, proof_m2) == AIRPLAY_SRP6A_STATE_ERROR);
    airplay_srp6a_server_destroy(server);

    assert(airplay_srp6a_server_start(username, pairing_code, failing_random,
                                      NULL, &server, salt, public_b) == AIRPLAY_SRP6A_RANDOM_FAILED);
    assert(server == NULL);
    puts("airplay srp6a host test passed");
    return 0;
}
