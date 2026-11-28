/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "airplay_mfi_sap.h"
#include "mbedtls/asn1write.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/x509_crt.h"
#include "rtcp/mfi_software.h"
#include "rtcp/rtcp.h"
#include "rtcp_linux.h"

typedef struct
{
    uint8_t key[RTCP_MFI_SOFTWARE_KEY_MAX];
    uint8_t cert[RTCP_MFI_WIRE_CERT_MAX];
    size_t key_len;
    size_t cert_len;
} identity_t;

static int secure_random(void *context, uint8_t *out, size_t len)
{
    if (context != NULL && !*(const bool *)context)
        return -1;
    return rtcp_os_linux.random(out, len) ? 0 : -1;
}

static bool sap_random(uint8_t *out, size_t len, void *context)
{
    return secure_random(context, out, len) == 0;
}

static void generate_identity(identity_t *identity, bool rsa, bool certificate_authority)
{
    mbedtls_pk_context key;
    mbedtls_x509write_cert writer;
    mbedtls_mpi serial;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&writer);
    mbedtls_mpi_init(&serial);
    assert(mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(
                                      rsa ? MBEDTLS_PK_RSA : MBEDTLS_PK_ECKEY)) == 0);
    if (rsa)
        assert(mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), secure_random, NULL, 1024, 65537) == 0);
    else
        assert(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                                   secure_random, NULL) == 0);
    mbedtls_x509write_crt_set_subject_key(&writer, &key);
    mbedtls_x509write_crt_set_issuer_key(&writer, &key);
    mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
    assert(mbedtls_mpi_lset(&serial, 1) == 0);
    assert(mbedtls_x509write_crt_set_serial(&writer, &serial) == 0);
    assert(mbedtls_x509write_crt_set_subject_name(&writer, "CN=rt-carplay host test") == 0);
    assert(mbedtls_x509write_crt_set_issuer_name(&writer, "CN=rt-carplay host test") == 0);
    assert(mbedtls_x509write_crt_set_validity(&writer, "20260101000000", "20360101000000") == 0);
    assert(mbedtls_x509write_crt_set_basic_constraints(&writer, certificate_authority, -1) == 0);
    assert(mbedtls_x509write_crt_set_key_usage(&writer, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0);

    int len = mbedtls_pk_write_key_der(&key, identity->key, sizeof(identity->key));
    assert(len > 0);
    identity->key_len = (size_t)len;
    size_t offset = sizeof(identity->key) - identity->key_len;
    assert(offset >= identity->key_len);
    memcpy(identity->key, identity->key + offset, identity->key_len);
    mbedtls_platform_zeroize(identity->key + offset, identity->key_len);

    len = mbedtls_x509write_crt_der(&writer, identity->cert, sizeof(identity->cert),
                                    secure_random, NULL);
    assert(len > 0);
    identity->cert_len = (size_t)len;
    memmove(identity->cert, identity->cert + sizeof(identity->cert) - identity->cert_len,
            identity->cert_len);
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&writer);
    mbedtls_pk_free(&key);
}

static void put_bytes(uint8_t **cursor, const uint8_t *bytes, size_t len)
{
    *cursor -= len;
    memcpy(*cursor, bytes, len);
}

static void der_wrap(uint8_t **cursor, uint8_t *start, size_t len, uint8_t tag)
{
    assert(mbedtls_asn1_write_len(cursor, start, len) > 0);
    assert(mbedtls_asn1_write_tag(cursor, start, tag) == 1);
}

static size_t encode_pkcs7(const identity_t *leaf, const identity_t *authority,
                           uint8_t out[RTCP_MFI_WIRE_CERT_MAX])
{
    static const uint8_t signer_infos[] = { 0x31, 0x00 };
    static const uint8_t signed_data_prefix[] = {
        0x02,
        0x01,
        0x01,
        0x31,
        0x00,
        0x30,
        0x0b,
        0x06,
        0x09,
        0x2a,
        0x86,
        0x48,
        0x86,
        0xf7,
        0x0d,
        0x01,
        0x07,
        0x01,
    };
    static const uint8_t content_type[] = {
        0x06,
        0x09,
        0x2a,
        0x86,
        0x48,
        0x86,
        0xf7,
        0x0d,
        0x01,
        0x07,
        0x02,
    };
    uint8_t *end = out + RTCP_MFI_WIRE_CERT_MAX;
    uint8_t *cursor = end;
    assert(leaf->cert_len + authority->cert_len < RTCP_MFI_WIRE_CERT_MAX - 64U);
    put_bytes(&cursor, signer_infos, sizeof(signer_infos));
    put_bytes(&cursor, leaf->cert, leaf->cert_len);
    put_bytes(&cursor, authority->cert, authority->cert_len);
    der_wrap(&cursor, out, leaf->cert_len + authority->cert_len, 0xa0);
    put_bytes(&cursor, signed_data_prefix, sizeof(signed_data_prefix));
    der_wrap(&cursor, out, (size_t)(end - cursor), 0x30);
    der_wrap(&cursor, out, (size_t)(end - cursor), 0xa0);
    put_bytes(&cursor, content_type, sizeof(content_type));
    der_wrap(&cursor, out, (size_t)(end - cursor), 0x30);
    size_t len = (size_t)(end - cursor);
    memmove(out, cursor, len);
    return len;
}

static rtcp_mfi_software_config_t configuration(const identity_t *identity)
{
    return (rtcp_mfi_software_config_t){
        .os = &rtcp_os_linux,
        .certificate = identity->cert,
        .certificate_len = identity->cert_len,
        .private_key = identity->key,
        .private_key_len = identity->key_len,
    };
}

typedef struct
{
    const rtcp_mfi_provider_t *provider;
    const identity_t *identity;
    uint8_t major;
} signer_t;

static int verify_p256(mbedtls_ecp_keypair *key, const uint8_t digest[32],
                       const uint8_t signature[64])
{
    mbedtls_mpi first;
    mbedtls_mpi second;
    mbedtls_mpi_init(&first);
    mbedtls_mpi_init(&second);
    assert(mbedtls_mpi_read_binary(&first, signature, 32U) == 0);
    assert(mbedtls_mpi_read_binary(&second, signature + 32U, 32U) == 0);
    int result = mbedtls_ecdsa_verify(&key->grp, digest, 32U, &key->Q, &first, &second);
    mbedtls_mpi_free(&first);
    mbedtls_mpi_free(&second);
    return result;
}

static void *exercise_signer(void *context)
{
    const signer_t *signer = context;
    const rtcp_mfi_provider_t *provider = signer->provider;
    mbedtls_x509_crt cert;
    mbedtls_x509_crt_init(&cert);
    assert(mbedtls_x509_crt_parse_der(&cert, signer->identity->cert,
                                      signer->identity->cert_len) == 0);
    for (unsigned iteration = 0; iteration < 3U; ++iteration)
    {
        uint8_t digest[32];
        assert(secure_random(NULL, digest, sizeof(digest)) == 0);
        uint8_t signature[256];
        size_t len = 0U;
        assert(provider->sign(provider->context, digest, signer->major == 2U ? 20U : 32U,
                              signature, sizeof(signature), &len) == RTCP_OK);
        assert(len == (signer->major == 2U ? 128U : 64U));
        if (signer->major == 2U)
        {
            assert(mbedtls_pk_verify(&cert.pk, MBEDTLS_MD_SHA1, digest, 20U, signature, len) == 0);
            signature[0] ^= 1U;
            assert(mbedtls_pk_verify(&cert.pk, MBEDTLS_MD_SHA1, digest, 20U, signature, len) != 0);
        }
        else
        {
            assert(verify_p256(mbedtls_pk_ec(cert.pk), digest, signature) == 0);
            signature[0] ^= 1U;
            assert(verify_p256(mbedtls_pk_ec(cert.pk), digest, signature) != 0);
        }
    }
    mbedtls_x509_crt_free(&cert);
    return NULL;
}

static void assert_sap_identity(const rtcp_mfi_provider_t *provider, const uint8_t *expected,
                                size_t expected_len)
{
    static const uint8_t request[] = {
        1,
        0x85,
        0x20,
        0xf0,
        0x09,
        0x89,
        0x30,
        0xa7,
        0x54,
        0x74,
        0x8b,
        0x7d,
        0xdc,
        0xb4,
        0x3e,
        0xf7,
        0x5a,
        0x0d,
        0xbf,
        0x3a,
        0x0d,
        0x26,
        0x38,
        0x1a,
        0xf4,
        0xeb,
        0xa4,
        0xa9,
        0x8e,
        0xaa,
        0x9b,
        0x4e,
        0x6a,
    };
    uint8_t response[AIRPLAY_MFI_SAP_MAX_RESPONSE_BYTES];
    size_t len = 0U;
    assert(airplay_mfi_sap_respond_with_provider(provider, sap_random, NULL, request,
                                                 sizeof(request), response, sizeof(response), &len) == AIRPLAY_MFI_SAP_OK);
    assert(len > 36U + expected_len);
    assert(memcmp(response + 36U, expected, expected_len) == 0);
    assert(airplay_mfi_sap_respond_with_provider(provider, NULL, NULL, request,
                                                 sizeof(request), response, sizeof(response), &len) == AIRPLAY_MFI_SAP_CRYPTO_FAILED);
    assert(len == 0U);
}

int main(void)
{
    identity_t rsa;
    identity_t ecc;
    identity_t authority;
    generate_identity(&rsa, true, false);
    generate_identity(&ecc, false, false);
    generate_identity(&authority, false, true);
    uint8_t pkcs7[RTCP_MFI_WIRE_CERT_MAX];
    size_t pkcs7_len = encode_pkcs7(&ecc, &authority, pkcs7);
    bool random_ready = true;
    rtcp_mfi_software_config_t configs[] = { configuration(&rsa), configuration(&ecc) };
    configs[1].certificate = pkcs7;
    configs[1].certificate_len = pkcs7_len;
    configs[1].random = secure_random;
    configs[1].random_context = &random_ready;

    rtcp_mfi_software_t *software[2] = { NULL, NULL };
    signer_t signers[2];
    for (size_t index = 0U; index < 2U; ++index)
    {
        assert(rtcp_mfi_software_create(&configs[index], &software[index]) == RTCP_OK);
        signers[index] = (signer_t){
            .provider = rtcp_mfi_software_provider(software[index]),
            .identity = index == 0U ? &rsa : &ecc,
            .major = (uint8_t)(index + 2U),
        };
        rtcp_mfi_provider_info_t info;
        assert(signers[index].provider->get_info(signers[index].provider->context, &info) == RTCP_OK);
        assert(info.kind == RTCP_MFI_PROVIDER_SOFTWARE && info.protocol_major == signers[index].major);
        assert(info.capabilities == (RTCP_MFI_CAP_IAP2 | RTCP_MFI_CAP_SAP));
        assert(info.certificate_len == configs[index].certificate_len);
        assert_sap_identity(signers[index].provider, configs[index].certificate,
                            configs[index].certificate_len);
    }

    pthread_t threads[4];
    for (size_t index = 0U; index < 4U; ++index)
        assert(pthread_create(&threads[index], NULL, exercise_signer, &signers[index % 2U]) == 0);
    for (size_t index = 0U; index < 4U; ++index)
        assert(pthread_join(threads[index], NULL) == 0);

    const rtcp_mfi_provider_t *provider = signers[1].provider;
    uint8_t digest[32] = { 0 };
    uint8_t signature[64];
    size_t len = 99U;
    assert(provider->sign(provider->context, digest, 20U, signature, sizeof(signature), &len) == RTCP_EINVAL);
    assert(provider->sign(provider->context, digest, 32U, signature, 63U, &len) == RTCP_ENOMEM);
    random_ready = false;
    assert(provider->sign(provider->context, digest, 32U, signature, sizeof(signature), &len) == RTCP_EIO);
    assert(len == 0U);
    assert(provider->read_cert(provider->context, signature, sizeof(signature), &len) == RTCP_ENOMEM);

    rtcp_mfi_software_t *bad = software[0];
    rtcp_mfi_software_config_t invalid = configuration(&authority);
    assert(rtcp_mfi_software_create(&invalid, &bad) == RTCP_EINVAL && bad == NULL);
    invalid = configs[1];
    invalid.private_key = authority.key;
    invalid.private_key_len = authority.key_len;
    assert(rtcp_mfi_software_create(&invalid, &bad) == RTCP_EINVAL && bad == NULL);
    invalid = configs[0];
    invalid.protocol_major = 3U;
    assert(rtcp_mfi_software_create(&invalid, &bad) == RTCP_ENOTSUP && bad == NULL);
    invalid = configs[0];
    invalid.private_key_len = 0;
    assert(rtcp_mfi_software_create(&invalid, &bad) == RTCP_EINVAL && bad == NULL);
    invalid = configs[1];
    --invalid.certificate_len;
    assert(rtcp_mfi_software_create(&invalid, &bad) == RTCP_EINVAL && bad == NULL);

    for (size_t index = 0U; index < 2U; ++index)
        rtcp_mfi_software_destroy(software[index]);
    mbedtls_platform_zeroize(&rsa, sizeof(rsa));
    mbedtls_platform_zeroize(&ecc, sizeof(ecc));
    mbedtls_platform_zeroize(&authority, sizeof(authority));
    puts("classic MFi software identity, SAP and concurrent signing passed");
    return 0;
}
