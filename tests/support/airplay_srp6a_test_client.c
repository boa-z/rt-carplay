/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Client-side SRP-6a proof generator used by the Pair-Setup tests.
 * Ported from esp32-carplay-web-relay tools/airplay_srp6a_test_client.h;
 * it is test scaffolding, not production code.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "airplay_srp6a.h"
#include "airplay_srp6a_test_client.h"
#include "mbedtls/bignum.h"
#include "mbedtls/md.h"

#define GROUP_BYTES AIRPLAY_SRP6A_PUBLIC_BYTES

static const char *const N_HEX =
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

typedef struct {
    const uint8_t *bytes;
    size_t len;
} part_t;

static int sha512(const part_t *parts, size_t count, uint8_t digest[64])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    assert(info != NULL);
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    int rc = mbedtls_md_setup(&md, info, 0);
    if (rc == 0) rc = mbedtls_md_starts(&md);
    for (size_t i = 0; rc == 0 && i < count; ++i) {
        if (parts[i].len != 0) rc = mbedtls_md_update(&md, parts[i].bytes, parts[i].len);
    }
    if (rc == 0) rc = mbedtls_md_finish(&md, digest);
    mbedtls_md_free(&md);
    return rc;
}

static int write_padded(const mbedtls_mpi *mpi, uint8_t output[GROUP_BYTES])
{
    assert(mbedtls_mpi_size(mpi) <= GROUP_BYTES);
    return mbedtls_mpi_write_binary(mpi, output, GROUP_BYTES);
}

static int write_minimal(const mbedtls_mpi *mpi, uint8_t *output,
                         size_t capacity, size_t *written)
{
    size_t size = mbedtls_mpi_size(mpi);
    if (size == 0) size = 1;
    if (size > capacity) return -1;
    int rc = mbedtls_mpi_write_binary(mpi, output, size);
    if (rc == 0) *written = size;
    return rc;
}

void airplay_srp6a_test_compute_client_proofs(
    const uint8_t salt[AIRPLAY_SRP6A_SALT_BYTES],
    const uint8_t public_b_bytes[GROUP_BYTES],
    const char *username,
    const char *password,
    uint8_t public_a_bytes[GROUP_BYTES],
    uint8_t proof_m1[64],
    uint8_t expected_m2[64],
    uint8_t expected_key[64])
{
    mbedtls_mpi n, g, k, x, verifier, a, public_a, public_b, u, tmp, base, exponent, shared_s;
    mbedtls_mpi_init(&n); mbedtls_mpi_init(&g); mbedtls_mpi_init(&k); mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&verifier); mbedtls_mpi_init(&a); mbedtls_mpi_init(&public_a);
    mbedtls_mpi_init(&public_b); mbedtls_mpi_init(&u); mbedtls_mpi_init(&tmp);
    mbedtls_mpi_init(&base); mbedtls_mpi_init(&exponent); mbedtls_mpi_init(&shared_s);

    uint8_t n_bytes[GROUP_BYTES] = {0}, g_bytes[GROUP_BYTES] = {0};
    uint8_t g_pad[GROUP_BYTES] = {0};
    uint8_t salt_inner[64] = {0}, digest[64] = {0}, a_pad[GROUP_BYTES] = {0};
    uint8_t b_pad[GROUP_BYTES] = {0}, session_bytes[GROUP_BYTES] = {0};
    uint8_t h_n[64] = {0}, h_g[64] = {0}, h_i[64] = {0}, xor_h[64] = {0};
    size_t value_len = 0;
    static const uint8_t colon = ':';
    static const uint8_t client_private_a[32] = {
        0x13, 0x88, 0x21, 0x44, 0x92, 0x7a, 0x6d, 0x09,
        0x37, 0x55, 0xaa, 0x81, 0x03, 0x5c, 0x9e, 0x20,
        0x61, 0x73, 0x0f, 0x91, 0x28, 0x42, 0xbb, 0xc4,
        0x77, 0x18, 0xe2, 0x0a, 0x49, 0xd3, 0x66, 0x5f,
    };

    assert(mbedtls_mpi_read_string(&n, 16, N_HEX) == 0);
    assert(mbedtls_mpi_lset(&g, 5) == 0);
    assert(write_padded(&n, n_bytes) == 0);
    assert(write_padded(&g, g_pad) == 0);
    part_t k_parts[] = {{n_bytes, sizeof(n_bytes)}, {g_pad, sizeof(g_pad)}};
    assert(sha512(k_parts, 2, digest) == 0);
    assert(mbedtls_mpi_read_binary(&k, digest, sizeof(digest)) == 0);
    assert(mbedtls_mpi_read_binary(&public_b, public_b_bytes, GROUP_BYTES) == 0);

    part_t inner_parts[] = {
        {(const uint8_t *)username, strlen(username)},
        {&colon, 1},
        {(const uint8_t *)password, strlen(password)},
    };
    assert(sha512(inner_parts, 3, salt_inner) == 0);
    part_t x_parts[] = {{salt, AIRPLAY_SRP6A_SALT_BYTES}, {salt_inner, sizeof(salt_inner)}};
    assert(sha512(x_parts, 2, digest) == 0);
    assert(mbedtls_mpi_read_binary(&x, digest, sizeof(digest)) == 0);
    assert(mbedtls_mpi_exp_mod(&verifier, &g, &x, &n, NULL) == 0);

    assert(mbedtls_mpi_read_binary(&a, client_private_a, sizeof(client_private_a)) == 0);
    assert(mbedtls_mpi_exp_mod(&public_a, &g, &a, &n, NULL) == 0);
    assert(write_padded(&public_a, a_pad) == 0);
    assert(write_padded(&public_a, public_a_bytes) == 0);
    assert(write_padded(&public_b, b_pad) == 0);
    part_t u_parts[] = {{a_pad, sizeof(a_pad)}, {b_pad, sizeof(b_pad)}};
    assert(sha512(u_parts, 2, digest) == 0);
    assert(mbedtls_mpi_read_binary(&u, digest, sizeof(digest)) == 0);

    assert(mbedtls_mpi_mul_mpi(&tmp, &k, &verifier) == 0);
    assert(mbedtls_mpi_mod_mpi(&tmp, &tmp, &n) == 0);
    assert(mbedtls_mpi_sub_mpi(&base, &public_b, &tmp) == 0);
    assert(mbedtls_mpi_mod_mpi(&base, &base, &n) == 0);
    assert(mbedtls_mpi_mul_mpi(&exponent, &u, &x) == 0);
    assert(mbedtls_mpi_add_mpi(&exponent, &exponent, &a) == 0);
    assert(mbedtls_mpi_exp_mod(&shared_s, &base, &exponent, &n, NULL) == 0);
    assert(write_minimal(&shared_s, session_bytes, sizeof(session_bytes), &value_len) == 0);
    part_t key_parts[] = {{session_bytes, value_len}};
    assert(sha512(key_parts, 1, expected_key) == 0);

    assert(write_minimal(&n, n_bytes, sizeof(n_bytes), &value_len) == 0);
    part_t h_n_parts[] = {{n_bytes, value_len}};
    assert(sha512(h_n_parts, 1, h_n) == 0);
    assert(write_minimal(&g, g_bytes, sizeof(g_bytes), &value_len) == 0);
    part_t h_g_parts[] = {{g_bytes, value_len}};
    assert(sha512(h_g_parts, 1, h_g) == 0);
    for (size_t i = 0; i < sizeof(xor_h); ++i) xor_h[i] = (uint8_t)(h_n[i] ^ h_g[i]);
    part_t h_i_parts[] = {{(const uint8_t *)username, strlen(username)}};
    assert(sha512(h_i_parts, 1, h_i) == 0);
    part_t m1_parts[] = {
        {xor_h, sizeof(xor_h)}, {h_i, sizeof(h_i)}, {salt, AIRPLAY_SRP6A_SALT_BYTES},
        {a_pad, sizeof(a_pad)}, {b_pad, sizeof(b_pad)}, {expected_key, 64},
    };
    assert(sha512(m1_parts, sizeof(m1_parts) / sizeof(m1_parts[0]), proof_m1) == 0);
    part_t m2_parts[] = {{a_pad, sizeof(a_pad)}, {proof_m1, 64}, {expected_key, 64}};
    assert(sha512(m2_parts, sizeof(m2_parts) / sizeof(m2_parts[0]), expected_m2) == 0);

    mbedtls_mpi_free(&n); mbedtls_mpi_free(&g); mbedtls_mpi_free(&k); mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&verifier); mbedtls_mpi_free(&a); mbedtls_mpi_free(&public_a);
    mbedtls_mpi_free(&public_b); mbedtls_mpi_free(&u); mbedtls_mpi_free(&tmp);
    mbedtls_mpi_free(&base); mbedtls_mpi_free(&exponent); mbedtls_mpi_free(&shared_s);
}
