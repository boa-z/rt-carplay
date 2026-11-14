/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 ESP32 CarPlay Web Relay contributors
 * The SRP-6a equations and RFC 5054 group follow DiPlay's Srp6a.kt.
 * See THIRD_PARTY_NOTICES.md.
 */
#include "airplay_srp6a.h"

#include <stdlib.h>
#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/md.h"

#define SRP_USERNAME_MAX_BYTES 64U
#define SRP_PAIRING_CODE_MAX_BYTES 64U
#define SRP_PRIVATE_BYTES 32U

static const char *const SRP_N_HEX =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183"
    "995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A"
    "85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7AB"
    "F5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D8"
    "7602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208"
    "E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

struct airplay_srp6a_server {
    mbedtls_mpi n;
    mbedtls_mpi g;
    mbedtls_mpi multiplier;
    mbedtls_mpi verifier;
    mbedtls_mpi private_b;
    mbedtls_mpi public_b;
    uint8_t salt[AIRPLAY_SRP6A_SALT_BYTES];
    char username[SRP_USERNAME_MAX_BYTES + 1U];
    bool spent;
};

static airplay_srp6a_server_t *srp_server_calloc(void)
{
    return calloc(1U, sizeof(airplay_srp6a_server_t));
}

static void srp_server_free(airplay_srp6a_server_t *server)
{
    free(server);
}

typedef struct {
    const uint8_t *data;
    size_t len;
} hash_part_t;

static int sha512_parts(const hash_part_t *parts, size_t part_count, uint8_t output[64])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    if (info == NULL) return -1;
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    int rc = mbedtls_md_setup(&md, info, 0);
    if (rc == 0) rc = mbedtls_md_starts(&md);
    for (size_t i = 0; rc == 0 && i < part_count; ++i) {
        if (parts[i].len != 0) {
            if (parts[i].data == NULL) rc = -1;
            else rc = mbedtls_md_update(&md, parts[i].data, parts[i].len);
        }
    }
    if (rc == 0) rc = mbedtls_md_finish(&md, output);
    mbedtls_md_free(&md);
    return rc;
}

static int mpi_write_padded(const mbedtls_mpi *value, uint8_t output[AIRPLAY_SRP6A_PUBLIC_BYTES])
{
    if (mbedtls_mpi_size(value) > AIRPLAY_SRP6A_PUBLIC_BYTES) return -1;
    return mbedtls_mpi_write_binary(value, output, AIRPLAY_SRP6A_PUBLIC_BYTES);
}

static int mpi_write_minimal(const mbedtls_mpi *value, uint8_t *output,
                             size_t capacity, size_t *output_len)
{
    size_t len = mbedtls_mpi_size(value);
    if (len == 0) len = 1;
    if (len > capacity) return -1;
    int rc = mbedtls_mpi_write_binary(value, output, len);
    if (rc == 0) *output_len = len;
    return rc;
}

static bool bytes_equal_constant_time(const uint8_t *left, const uint8_t *right, size_t len)
{
    volatile uint8_t difference = 0;
    for (size_t i = 0; i < len; ++i) difference |= (uint8_t)(left[i] ^ right[i]);
    return difference == 0;
}

static void secure_zero(void *buffer, size_t len)
{
    volatile uint8_t *bytes = (volatile uint8_t *)buffer;
    while (len-- != 0) *bytes++ = 0;
}

void airplay_srp6a_server_destroy(airplay_srp6a_server_t *server)
{
    if (server == NULL) return;
    mbedtls_mpi_free(&server->n);
    mbedtls_mpi_free(&server->g);
    mbedtls_mpi_free(&server->multiplier);
    mbedtls_mpi_free(&server->verifier);
    mbedtls_mpi_free(&server->private_b);
    mbedtls_mpi_free(&server->public_b);
    secure_zero(server, sizeof(*server));
    srp_server_free(server);
}

airplay_srp6a_result_t airplay_srp6a_server_start(const char *username,
                                                  const char *pairing_code,
                                                  airplay_srp6a_random_cb_t random_cb,
                                                  void *random_context,
                                                  airplay_srp6a_server_t **out_server,
                                                  uint8_t out_salt[AIRPLAY_SRP6A_SALT_BYTES],
                                                  uint8_t out_public_b[AIRPLAY_SRP6A_PUBLIC_BYTES])
{
    if (out_server != NULL) *out_server = NULL;
    if (out_salt != NULL) memset(out_salt, 0, AIRPLAY_SRP6A_SALT_BYTES);
    if (out_public_b != NULL) memset(out_public_b, 0, AIRPLAY_SRP6A_PUBLIC_BYTES);
    if (username == NULL || pairing_code == NULL || random_cb == NULL || out_server == NULL ||
        out_salt == NULL || out_public_b == NULL) return AIRPLAY_SRP6A_INVALID_ARGUMENT;
    size_t username_len = strlen(username);
    size_t code_len = strlen(pairing_code);
    if (username_len == 0 || username_len > SRP_USERNAME_MAX_BYTES ||
        code_len == 0 || code_len > SRP_PAIRING_CODE_MAX_BYTES) {
        return AIRPLAY_SRP6A_INVALID_ARGUMENT;
    }

    airplay_srp6a_server_t *server = srp_server_calloc();
    if (server == NULL) return AIRPLAY_SRP6A_NO_MEMORY;
    mbedtls_mpi_init(&server->n);
    mbedtls_mpi_init(&server->g);
    mbedtls_mpi_init(&server->multiplier);
    mbedtls_mpi_init(&server->verifier);
    mbedtls_mpi_init(&server->private_b);
    mbedtls_mpi_init(&server->public_b);
    memcpy(server->username, username, username_len);
    uint8_t random_bytes[AIRPLAY_SRP6A_SALT_BYTES + SRP_PRIVATE_BYTES] = {0};
    uint8_t n_padded[AIRPLAY_SRP6A_PUBLIC_BYTES] = {0};
    uint8_t g_padded[AIRPLAY_SRP6A_PUBLIC_BYTES] = {0};
    uint8_t digest[64] = {0};
    uint8_t inner[64] = {0};
    uint8_t b_public[AIRPLAY_SRP6A_PUBLIC_BYTES] = {0};
    mbedtls_mpi x, temporary, g_to_b, hash_k;
    mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&temporary);
    mbedtls_mpi_init(&g_to_b);
    mbedtls_mpi_init(&hash_k);

    bool random_failed = false;
    int rc = mbedtls_mpi_read_string(&server->n, 16, SRP_N_HEX);
    if (rc == 0) rc = mbedtls_mpi_lset(&server->g, 5);
    if (rc == 0) rc = mpi_write_padded(&server->n, n_padded);
    if (rc == 0) rc = mpi_write_padded(&server->g, g_padded);
    if (rc == 0) {
        const hash_part_t parts[] = {
            {.data = n_padded, .len = sizeof(n_padded)},
            {.data = g_padded, .len = sizeof(g_padded)},
        };
        rc = sha512_parts(parts, sizeof(parts) / sizeof(parts[0]), digest);
    }
    if (rc == 0) rc = mbedtls_mpi_read_binary(&hash_k, digest, sizeof(digest));
    if (rc == 0) rc = mbedtls_mpi_copy(&server->multiplier, &hash_k);
    if (rc == 0 && !random_cb(random_bytes, sizeof(random_bytes), random_context)) {
        random_failed = true;
        rc = -1;
    }
    if (rc == 0) {
        memcpy(server->salt, random_bytes, AIRPLAY_SRP6A_SALT_BYTES);
        rc = mbedtls_mpi_read_binary(&server->private_b,
                                     random_bytes + AIRPLAY_SRP6A_SALT_BYTES,
                                     SRP_PRIVATE_BYTES);
    }
    if (rc == 0 && mbedtls_mpi_cmp_int(&server->private_b, 0) == 0) rc = -1;
    if (rc == 0) {
        static const uint8_t colon = ':';
        const hash_part_t inner_parts[] = {
            {.data = (const uint8_t *)server->username, .len = username_len},
            {.data = &colon, .len = 1},
            {.data = (const uint8_t *)pairing_code, .len = code_len},
        };
        rc = sha512_parts(inner_parts, sizeof(inner_parts) / sizeof(inner_parts[0]), inner);
    }
    if (rc == 0) {
        const hash_part_t x_parts[] = {
            {.data = server->salt, .len = sizeof(server->salt)},
            {.data = inner, .len = sizeof(inner)},
        };
        rc = sha512_parts(x_parts, sizeof(x_parts) / sizeof(x_parts[0]), digest);
    }
    if (rc == 0) rc = mbedtls_mpi_read_binary(&x, digest, sizeof(digest));
    if (rc == 0) rc = mbedtls_mpi_exp_mod(&server->verifier, &server->g, &x, &server->n, NULL);
    if (rc == 0) rc = mbedtls_mpi_exp_mod(&g_to_b, &server->g, &server->private_b,
                                          &server->n, NULL);
    if (rc == 0) rc = mbedtls_mpi_mul_mpi(&temporary, &server->multiplier, &server->verifier);
    if (rc == 0) rc = mbedtls_mpi_mod_mpi(&temporary, &temporary, &server->n);
    if (rc == 0) rc = mbedtls_mpi_add_mpi(&temporary, &temporary, &g_to_b);
    if (rc == 0) rc = mbedtls_mpi_mod_mpi(&server->public_b, &temporary, &server->n);
    if (rc == 0 && mbedtls_mpi_cmp_int(&server->public_b, 0) == 0) rc = -1;
    if (rc == 0) rc = mpi_write_padded(&server->public_b, b_public);

    mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&temporary);
    mbedtls_mpi_free(&g_to_b);
    mbedtls_mpi_free(&hash_k);
    secure_zero(random_bytes, sizeof(random_bytes));
    secure_zero(n_padded, sizeof(n_padded));
    secure_zero(g_padded, sizeof(g_padded));
    secure_zero(digest, sizeof(digest));
    secure_zero(inner, sizeof(inner));

    if (rc != 0) {
        secure_zero(b_public, sizeof(b_public));
        airplay_srp6a_server_destroy(server);
        return random_failed ? AIRPLAY_SRP6A_RANDOM_FAILED : AIRPLAY_SRP6A_CRYPTO_FAILED;
    }
    memcpy(out_salt, server->salt, sizeof(server->salt));
    memcpy(out_public_b, b_public, sizeof(b_public));
    secure_zero(b_public, sizeof(b_public));
    *out_server = server;
    return AIRPLAY_SRP6A_OK;
}

airplay_srp6a_result_t airplay_srp6a_server_verify(airplay_srp6a_server_t *server,
                                                   const uint8_t *public_a,
                                                   size_t public_a_len,
                                                   const uint8_t *proof_m1,
                                                   size_t proof_m1_len,
                                                   uint8_t out_session_key[AIRPLAY_SRP6A_SESSION_KEY_BYTES],
                                                   uint8_t out_proof_m2[AIRPLAY_SRP6A_PROOF_BYTES])
{
    if (out_session_key != NULL) memset(out_session_key, 0, AIRPLAY_SRP6A_SESSION_KEY_BYTES);
    if (out_proof_m2 != NULL) memset(out_proof_m2, 0, AIRPLAY_SRP6A_PROOF_BYTES);
    if (server == NULL || public_a == NULL || proof_m1 == NULL || out_session_key == NULL ||
        out_proof_m2 == NULL || public_a_len != AIRPLAY_SRP6A_PUBLIC_BYTES ||
        proof_m1_len != AIRPLAY_SRP6A_PROOF_BYTES) return AIRPLAY_SRP6A_INVALID_ARGUMENT;
    if (server->spent) return AIRPLAY_SRP6A_STATE_ERROR;
    server->spent = true;

    uint8_t a_padded[AIRPLAY_SRP6A_PUBLIC_BYTES] = {0};
    uint8_t b_padded[AIRPLAY_SRP6A_PUBLIC_BYTES] = {0};
    uint8_t n_bytes[AIRPLAY_SRP6A_PUBLIC_BYTES] = {0};
    uint8_t g_bytes[AIRPLAY_SRP6A_PUBLIC_BYTES] = {0};
    uint8_t h_n[64] = {0}, h_g[64] = {0}, h_i[64] = {0};
    uint8_t xor_h[64] = {0}, digest[64] = {0}, session_key[64] = {0};
    uint8_t expected_m1[64] = {0}, proof_m2[64] = {0};
    uint8_t s_bytes[AIRPLAY_SRP6A_PUBLIC_BYTES] = {0};
    size_t s_len = 0;
    mbedtls_mpi a, a_mod_n, u, v_to_u, base, shared_s;
    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&a_mod_n);
    mbedtls_mpi_init(&u);
    mbedtls_mpi_init(&v_to_u);
    mbedtls_mpi_init(&base);
    mbedtls_mpi_init(&shared_s);

    bool auth_failed = false;
    int rc = mbedtls_mpi_read_binary(&a, public_a, public_a_len);
    if (rc == 0) rc = mbedtls_mpi_mod_mpi(&a_mod_n, &a, &server->n);
    if (rc == 0 && mbedtls_mpi_cmp_int(&a_mod_n, 0) == 0) rc = -1;
    if (rc == 0) rc = mpi_write_padded(&a, a_padded);
    if (rc == 0) rc = mpi_write_padded(&server->public_b, b_padded);
    if (rc == 0) {
        const hash_part_t u_parts[] = {
            {.data = a_padded, .len = sizeof(a_padded)},
            {.data = b_padded, .len = sizeof(b_padded)},
        };
        rc = sha512_parts(u_parts, sizeof(u_parts) / sizeof(u_parts[0]), digest);
    }
    if (rc == 0) rc = mbedtls_mpi_read_binary(&u, digest, sizeof(digest));
    if (rc == 0 && mbedtls_mpi_cmp_int(&u, 0) == 0) rc = -1;
    if (rc == 0) rc = mbedtls_mpi_exp_mod(&v_to_u, &server->verifier, &u, &server->n, NULL);
    if (rc == 0) rc = mbedtls_mpi_mul_mpi(&base, &a, &v_to_u);
    if (rc == 0) rc = mbedtls_mpi_mod_mpi(&base, &base, &server->n);
    if (rc == 0 && mbedtls_mpi_cmp_int(&base, 0) == 0) rc = -1;
    if (rc == 0) rc = mbedtls_mpi_exp_mod(&shared_s, &base, &server->private_b,
                                          &server->n, NULL);
    if (rc == 0) rc = mpi_write_minimal(&shared_s, s_bytes, sizeof(s_bytes), &s_len);
    if (rc == 0) {
        const hash_part_t key_parts[] = {{.data = s_bytes, .len = s_len}};
        rc = sha512_parts(key_parts, 1, session_key);
    }
    if (rc == 0) rc = mpi_write_padded(&server->n, n_bytes);
    if (rc == 0) rc = mpi_write_minimal(&server->g, g_bytes, sizeof(g_bytes), &s_len);
    if (rc == 0) {
        const hash_part_t n_parts[] = {{.data = n_bytes, .len = sizeof(n_bytes)}};
        const hash_part_t g_parts[] = {{.data = g_bytes, .len = s_len}};
        rc = sha512_parts(n_parts, 1, h_n);
        if (rc == 0) rc = sha512_parts(g_parts, 1, h_g);
    }
    if (rc == 0) {
        for (size_t i = 0; i < sizeof(xor_h); ++i) xor_h[i] = (uint8_t)(h_n[i] ^ h_g[i]);
        const hash_part_t identifier_part[] = {
            {.data = (const uint8_t *)server->username, .len = strlen(server->username)},
        };
        rc = sha512_parts(identifier_part, 1, h_i);
    }
    if (rc == 0) {
        const hash_part_t proof_parts[] = {
            {.data = xor_h, .len = sizeof(xor_h)},
            {.data = h_i, .len = sizeof(h_i)},
            {.data = server->salt, .len = sizeof(server->salt)},
            {.data = a_padded, .len = sizeof(a_padded)},
            {.data = b_padded, .len = sizeof(b_padded)},
            {.data = session_key, .len = sizeof(session_key)},
        };
        rc = sha512_parts(proof_parts, sizeof(proof_parts) / sizeof(proof_parts[0]), expected_m1);
    }
    if (rc == 0 && !bytes_equal_constant_time(expected_m1, proof_m1, sizeof(expected_m1))) {
        auth_failed = true;
    }
    if (rc == 0 && !auth_failed) {
        const hash_part_t m2_parts[] = {
            {.data = a_padded, .len = sizeof(a_padded)},
            {.data = proof_m1, .len = proof_m1_len},
            {.data = session_key, .len = sizeof(session_key)},
        };
        rc = sha512_parts(m2_parts, sizeof(m2_parts) / sizeof(m2_parts[0]), proof_m2);
    }

    mbedtls_mpi_free(&a);
    mbedtls_mpi_free(&a_mod_n);
    mbedtls_mpi_free(&u);
    mbedtls_mpi_free(&v_to_u);
    mbedtls_mpi_free(&base);
    mbedtls_mpi_free(&shared_s);
    secure_zero(a_padded, sizeof(a_padded));
    secure_zero(b_padded, sizeof(b_padded));
    secure_zero(n_bytes, sizeof(n_bytes));
    secure_zero(g_bytes, sizeof(g_bytes));
    secure_zero(h_n, sizeof(h_n));
    secure_zero(h_g, sizeof(h_g));
    secure_zero(h_i, sizeof(h_i));
    secure_zero(xor_h, sizeof(xor_h));
    secure_zero(digest, sizeof(digest));
    secure_zero(expected_m1, sizeof(expected_m1));
    secure_zero(s_bytes, sizeof(s_bytes));

    if (auth_failed) {
        secure_zero(session_key, sizeof(session_key));
        secure_zero(proof_m2, sizeof(proof_m2));
        return AIRPLAY_SRP6A_AUTH_FAILED;
    }
    if (rc != 0) {
        secure_zero(session_key, sizeof(session_key));
        secure_zero(proof_m2, sizeof(proof_m2));
        return AIRPLAY_SRP6A_CRYPTO_FAILED;
    }
    memcpy(out_session_key, session_key, sizeof(session_key));
    memcpy(out_proof_m2, proof_m2, sizeof(proof_m2));
    secure_zero(session_key, sizeof(session_key));
    secure_zero(proof_m2, sizeof(proof_m2));
    return AIRPLAY_SRP6A_OK;
}
