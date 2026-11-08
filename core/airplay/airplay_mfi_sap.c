/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CarPlay /auth-setup MFiSAP responder following DiPlay's MfiSapAuthSetup.kt.
 * Certificate and signing operations are injected through
 * airplay_mfi_sap_set_provider().
 */
#include "airplay_mfi_sap.h"

#include <string.h>

#include "rtcp/log.h"
#include "rtcp/rtcp.h"
#include "mbedtls/aes.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

static airplay_mfi_sap_provider_t s_provider;
static bool s_provider_set;

bool airplay_mfi_sap_set_provider(const airplay_mfi_sap_provider_t *provider)
{
    if (provider == NULL || provider->mfi == NULL ||
        provider->mfi->read_cert == NULL || provider->mfi->sign == NULL ||
        (provider->protocol_major != 2U && provider->protocol_major != 3U)) {
        return false;
    }
    s_provider = *provider;
    s_provider_set = true;
    return true;
}

void airplay_mfi_sap_clear_provider(void)
{
    memset(&s_provider, 0, sizeof(s_provider));
    s_provider_set = false;
}

#define MFI_SAP_VERSION 1U
#define MFI_SAP_PUBLIC_KEY_BYTES 32U
#define MFI_SAP_SIGNATURE_MAX_BYTES AIRPLAY_MFI_SAP_MAX_SIGNATURE_BYTES
#define MFI_SAP_CERTIFICATE_CAPACITY AIRPLAY_MFI_SAP_MAX_CERTIFICATE_BYTES

#define TAG "mfi_sap"

static int mfi_sap_rng(void *context, unsigned char *output, size_t output_len)
{
    (void)context;
    if (output == NULL && output_len != 0U) return -1;
    if (s_provider.random == NULL) return -1;
    return s_provider.random(output, output_len, s_provider.context);
}

static bool bytes_are_zero(const uint8_t *bytes, size_t length)
{
    uint8_t aggregate = 0U;
    for (size_t i = 0U; i < length; ++i) aggregate |= bytes[i];
    return aggregate == 0U;
}

static int sha1(const uint8_t *input, size_t input_len, uint8_t output[20])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (info == NULL) return -1;
    return mbedtls_md(info, input, input_len, output);
}

static int sha256(const uint8_t *input, size_t input_len, uint8_t output[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) return -1;
    return mbedtls_md(info, input, input_len, output);
}

static void write_be32(uint8_t output[4], uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static airplay_mfi_sap_result_t mfi_sap_exchange(
    const uint8_t peer_public_key[MFI_SAP_PUBLIC_KEY_BYTES],
    uint8_t accessory_public_key[MFI_SAP_PUBLIC_KEY_BYTES],
    uint8_t shared_secret[MFI_SAP_PUBLIC_KEY_BYTES])
{
    airplay_mfi_sap_result_t result = AIRPLAY_MFI_SAP_CRYPTO_FAILED;
    mbedtls_ecp_group group;
    mbedtls_mpi private_key;
    mbedtls_ecp_point accessory_public;
    mbedtls_ecp_point peer_point;
    mbedtls_mpi shared;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&private_key);
    mbedtls_ecp_point_init(&accessory_public);
    mbedtls_ecp_point_init(&peer_point);
    mbedtls_mpi_init(&shared);

    size_t public_len = 0U;
    if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_CURVE25519) != 0 ||
        mbedtls_ecp_gen_keypair(&group, &private_key, &accessory_public,
                                mfi_sap_rng, NULL) != 0 ||
        mbedtls_ecp_point_write_binary(&group, &accessory_public,
                                       MBEDTLS_ECP_PF_UNCOMPRESSED, &public_len,
                                       accessory_public_key,
                                       MFI_SAP_PUBLIC_KEY_BYTES) != 0 ||
        public_len != MFI_SAP_PUBLIC_KEY_BYTES ||
        mbedtls_ecp_point_read_binary(&group, &peer_point, peer_public_key,
                                      MFI_SAP_PUBLIC_KEY_BYTES) != 0 ||
        mbedtls_ecdh_compute_shared(&group, &shared, &peer_point, &private_key,
                                    mfi_sap_rng, NULL) != 0 ||
        mbedtls_mpi_write_binary_le(&shared, shared_secret,
                                    MFI_SAP_PUBLIC_KEY_BYTES) != 0 ||
        bytes_are_zero(shared_secret, MFI_SAP_PUBLIC_KEY_BYTES)) {
        goto cleanup;
    }
    result = AIRPLAY_MFI_SAP_OK;

cleanup:
    mbedtls_ecp_group_free(&group);
    mbedtls_mpi_free(&private_key);
    mbedtls_ecp_point_free(&accessory_public);
    mbedtls_ecp_point_free(&peer_point);
    mbedtls_mpi_free(&shared);
    return result;
}

static int derive_aes_material(const uint8_t shared_secret[MFI_SAP_PUBLIC_KEY_BYTES],
                               uint8_t key[16], uint8_t iv[16])
{
    uint8_t input[7U + MFI_SAP_PUBLIC_KEY_BYTES];
    uint8_t digest[20];
    memcpy(input, "AES-KEY", 7U);
    memcpy(input + 7U, shared_secret, MFI_SAP_PUBLIC_KEY_BYTES);
    if (sha1(input, sizeof(input), digest) != 0) goto failed;
    memcpy(key, digest, 16U);

    memcpy(input, "AES-IV", 6U);
    memcpy(input + 6U, shared_secret, MFI_SAP_PUBLIC_KEY_BYTES);
    if (sha1(input, 6U + MFI_SAP_PUBLIC_KEY_BYTES, digest) != 0) goto failed;
    memcpy(iv, digest, 16U);
    mbedtls_platform_zeroize(input, sizeof(input));
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return 0;

failed:
    mbedtls_platform_zeroize(input, sizeof(input));
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return -1;
}

static int encrypt_signature(const uint8_t key[16], const uint8_t iv[16],
                             const uint8_t *signature, size_t signature_len,
                             uint8_t *encrypted)
{
    mbedtls_aes_context aes;
    uint8_t nonce_counter[16];
    uint8_t stream_block[16] = {0};
    size_t offset = 0U;
    memcpy(nonce_counter, iv, sizeof(nonce_counter));
    mbedtls_aes_init(&aes);
    int rc = mbedtls_aes_setkey_enc(&aes, key, 128U);
    if (rc == 0) {
        rc = mbedtls_aes_crypt_ctr(&aes, signature_len, &offset,
                                   nonce_counter, stream_block, signature,
                                   encrypted);
    }
    mbedtls_aes_free(&aes);
    mbedtls_platform_zeroize(nonce_counter, sizeof(nonce_counter));
    mbedtls_platform_zeroize(stream_block, sizeof(stream_block));
    return rc;
}

airplay_mfi_sap_result_t airplay_mfi_sap_respond(
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    if (response_len != NULL) *response_len = 0U;
    if (request == NULL || request_len != 1U + MFI_SAP_PUBLIC_KEY_BYTES ||
        request[0] != MFI_SAP_VERSION || response == NULL || response_len == NULL) {
        return AIRPLAY_MFI_SAP_INVALID_ARGUMENT;
    }
    if (!s_provider_set) return AIRPLAY_MFI_SAP_UNAVAILABLE;

    const uint8_t protocol_major = s_provider.protocol_major;
    if (protocol_major != 2U && protocol_major != 3U) {
        return AIRPLAY_MFI_SAP_UNSUPPORTED_PROTOCOL;
    }

    uint8_t certificate[MFI_SAP_CERTIFICATE_CAPACITY];
    size_t certificate_len = sizeof(certificate);
    int err = s_provider.mfi->read_cert(certificate, sizeof(certificate),
                                       &certificate_len);
    if (err != RTCP_OK || certificate_len == 0U ||
        certificate_len > sizeof(certificate)) {
        RTCP_LOGE(TAG, "auth_setup=certificate_unavailable error=%d", err);
        mbedtls_platform_zeroize(certificate, sizeof(certificate));
        return AIRPLAY_MFI_SAP_UNAVAILABLE;
    }
    size_t required = MFI_SAP_PUBLIC_KEY_BYTES + 4U + certificate_len +
                      4U + MFI_SAP_SIGNATURE_MAX_BYTES;
    if (required > response_capacity) {
        mbedtls_platform_zeroize(certificate, sizeof(certificate));
        return AIRPLAY_MFI_SAP_BUFFER_TOO_SMALL;
    }

    uint8_t accessory_public_key[MFI_SAP_PUBLIC_KEY_BYTES] = {0};
    uint8_t shared_secret[MFI_SAP_PUBLIC_KEY_BYTES] = {0};
    uint8_t derivation_key[16] = {0};
    uint8_t derivation_iv[16] = {0};
    uint8_t signature_input[MFI_SAP_PUBLIC_KEY_BYTES * 2U];
    uint8_t digest[32] = {0};
    uint8_t signature[MFI_SAP_SIGNATURE_MAX_BYTES] = {0};
    uint8_t encrypted_signature[MFI_SAP_SIGNATURE_MAX_BYTES] = {0};
    size_t signature_len = sizeof(signature);

    airplay_mfi_sap_result_t result = mfi_sap_exchange(
        request + 1U, accessory_public_key, shared_secret);
    if (result != AIRPLAY_MFI_SAP_OK ||
        derive_aes_material(shared_secret, derivation_key, derivation_iv) != 0) {
        result = AIRPLAY_MFI_SAP_CRYPTO_FAILED;
        goto cleanup;
    }

    memcpy(signature_input, accessory_public_key, MFI_SAP_PUBLIC_KEY_BYTES);
    memcpy(signature_input + MFI_SAP_PUBLIC_KEY_BYTES, request + 1U,
           MFI_SAP_PUBLIC_KEY_BYTES);
    const size_t digest_len = protocol_major == 2U ? 20U : 32U;
    int hash_result = protocol_major == 2U
                          ? sha1(signature_input, sizeof(signature_input), digest)
                          : sha256(signature_input, sizeof(signature_input), digest);
    if (hash_result != 0) {
        result = AIRPLAY_MFI_SAP_CRYPTO_FAILED;
        goto cleanup;
    }
    err = s_provider.mfi->sign(digest, digest_len, signature, sizeof(signature),
                               &signature_len);
    if (err != RTCP_OK || signature_len == 0U ||
        signature_len > MFI_SAP_SIGNATURE_MAX_BYTES) {
        RTCP_LOGE(TAG, "auth_setup=sign_failed protocol_major=%u error=%d signature_bytes=%u",
                  (unsigned)protocol_major, err, (unsigned)signature_len);
        result = AIRPLAY_MFI_SAP_SIGN_FAILED;
        goto cleanup;
    }
    if (encrypt_signature(derivation_key, derivation_iv, signature,
                          signature_len, encrypted_signature) != 0) {
        result = AIRPLAY_MFI_SAP_CRYPTO_FAILED;
        goto cleanup;
    }

    size_t offset = 0U;
    memcpy(response + offset, accessory_public_key, sizeof(accessory_public_key));
    offset += sizeof(accessory_public_key);
    write_be32(response + offset, (uint32_t)certificate_len);
    offset += 4U;
    memcpy(response + offset, certificate, certificate_len);
    offset += certificate_len;
    write_be32(response + offset, (uint32_t)signature_len);
    offset += 4U;
    memcpy(response + offset, encrypted_signature, signature_len);
    offset += signature_len;
    *response_len = offset;
    RTCP_LOGI(TAG, "auth_setup=completed protocol_major=%u certificate_bytes=%u",
              (unsigned)protocol_major, (unsigned)certificate_len);
    result = AIRPLAY_MFI_SAP_OK;

cleanup:
    mbedtls_platform_zeroize(certificate, sizeof(certificate));
    mbedtls_platform_zeroize(accessory_public_key, sizeof(accessory_public_key));
    mbedtls_platform_zeroize(shared_secret, sizeof(shared_secret));
    mbedtls_platform_zeroize(derivation_key, sizeof(derivation_key));
    mbedtls_platform_zeroize(derivation_iv, sizeof(derivation_iv));
    mbedtls_platform_zeroize(signature_input, sizeof(signature_input));
    mbedtls_platform_zeroize(digest, sizeof(digest));
    mbedtls_platform_zeroize(signature, sizeof(signature));
    mbedtls_platform_zeroize(encrypted_signature, sizeof(encrypted_signature));
    return result;
}
