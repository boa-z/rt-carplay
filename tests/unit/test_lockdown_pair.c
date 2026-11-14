/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lockdown_pair.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"
#include "rtcp/rtcp.h"

/* A fixed PKCS#1 RSA public key, in the format the phone returns as the
 * DevicePublicKey data.  The matching private key is intentionally absent:
 * pairing only embeds the public key in a certificate. */
static const char device_public_key_pem[] =
    "-----BEGIN RSA PUBLIC KEY-----\n"
    "MIIBCgKCAQEA4jtAqhC/gkeGKQjAPhpvmNb3yQx/qj8I2JXViuwwuZVecE5pAoYn\n"
    "wO58YS1014QeEID5xcEHQD3m8m4zl64d2B1sDhegRhZozvR2yf1fSXMnTu7FQbL0\n"
    "B0ZSNRUOCWpdCLKvWd36ZykydZmbZnm4hGlN/CYhFM8QaaH3GGJ6D9ug3PPexM94\n"
    "/NUqmvcbDdY5qku+92kRQKL06AdfU6NUhQc2tBZL4f+dSHgLuHtOwZRaG3oiesAH\n"
    "fWpRCGGDmoccmNjiiXT8WrD5iaa8m3Re0Gb3QiXmrDGt3HiBqzcgeiocc3/wxQpS\n"
    "dbYW3XzLF6e0S6Dultfvu9zJAnY4vDidHwIDAQAB\n"
    "-----END RSA PUBLIC KEY-----\n";

static uint32_t rng_state = 0x12345678U;

static int test_rng(void *context, uint8_t *output, size_t length)
{
    (void)context;
    for (size_t i = 0U; i < length; ++i) {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        output[i] = (uint8_t)rng_state;
    }
    return 0;
}

static int failing_rng(void *context, uint8_t *output, size_t length)
{
    (void)context;
    (void)output;
    (void)length;
    return -1;
}

static void test_uuid(void)
{
    char uuid[RTCP_PAIR_UUID_CHARS];
    assert(rtcp_pair_generate_uuid(uuid, sizeof(uuid), test_rng, NULL) ==
           RTCP_OK);
    assert(strlen(uuid) == 36U);
    assert(uuid[8] == '-' && uuid[13] == '-' && uuid[18] == '-' &&
           uuid[23] == '-');
    assert(uuid[14] == '4');
    assert(uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'A' ||
           uuid[19] == 'B');
    for (size_t i = 0U; i < 36U; ++i) {
        bool dash = i == 8U || i == 13U || i == 18U || i == 23U;
        bool hex = (uuid[i] >= '0' && uuid[i] <= '9') ||
                   (uuid[i] >= 'A' && uuid[i] <= 'F');
        assert(dash || hex);
    }

    assert(rtcp_pair_generate_uuid(uuid, 36U, test_rng, NULL) == RTCP_EINVAL);
    assert(rtcp_pair_generate_uuid(uuid, sizeof(uuid), NULL, NULL) ==
           RTCP_EINVAL);
    assert(rtcp_pair_generate_uuid(uuid, sizeof(uuid), failing_rng, NULL) ==
           RTCP_EIO);
}

static void test_generate_and_verify(void)
{
    static rtcp_pair_record_t record;
    memset(&record, 0, sizeof(record));

    int ret = rtcp_pair_record_generate(
        &record, (const uint8_t *)device_public_key_pem,
        sizeof(device_public_key_pem) - 1U, "00:11:22:33:44:55",
        "11111111-2222-4333-8444-555555555555",
        "66666666-7777-4888-8999-AAAAAAAAAAAA", test_rng, NULL);
    assert(ret == RTCP_OK);

    assert(strcmp(record.wifi_address, "00:11:22:33:44:55") == 0);
    assert(record.device_public_key_length ==
           sizeof(device_public_key_pem) - 1U);
    assert(memcmp(record.device_public_key, device_public_key_pem,
                  record.device_public_key_length) == 0);

    assert(record.root_certificate_length > 0U);
    assert(record.host_certificate_length > 0U);
    assert(record.device_certificate_length > 0U);
    assert(record.root_private_key_length > 0U);
    assert(record.host_private_key_length > 0U);
    assert(strstr((const char *)record.root_certificate,
                  "-----BEGIN CERTIFICATE-----") != NULL);
    assert(strstr((const char *)record.root_private_key,
                  "-----BEGIN") != NULL);

    static mbedtls_x509_crt root;
    static mbedtls_x509_crt host;
    static mbedtls_x509_crt device;
    static mbedtls_pk_context root_key;
    static mbedtls_pk_context host_key;
    mbedtls_x509_crt_init(&root);
    mbedtls_x509_crt_init(&host);
    mbedtls_x509_crt_init(&device);
    mbedtls_pk_init(&root_key);
    mbedtls_pk_init(&host_key);

    assert(mbedtls_x509_crt_parse(&root, record.root_certificate,
                                  record.root_certificate_length + 1U) == 0);
    assert(mbedtls_x509_crt_parse(&host, record.host_certificate,
                                  record.host_certificate_length + 1U) == 0);
    assert(mbedtls_x509_crt_parse(&device, record.device_certificate,
                                  record.device_certificate_length + 1U) == 0);
    assert(root.ca_istrue == 1);
    assert(host.ca_istrue == 0);
    assert(device.ca_istrue == 0);

    uint32_t flags = 0xffffffffU;
    assert(mbedtls_x509_crt_verify(&host, &root, NULL, NULL, &flags, NULL,
                                   NULL) == 0);
    assert(flags == 0U);
    flags = 0xffffffffU;
    assert(mbedtls_x509_crt_verify(&device, &root, NULL, NULL, &flags, NULL,
                                   NULL) == 0);
    assert(flags == 0U);

    assert(mbedtls_pk_parse_key(&root_key, record.root_private_key,
                                record.root_private_key_length + 1U, NULL,
                                0U) == 0);
    assert(mbedtls_pk_parse_key(&host_key, record.host_private_key,
                                record.host_private_key_length + 1U, NULL,
                                0U) == 0);
    /* The root identity is what lockdown presents over TLS. */
    assert(mbedtls_pk_check_pair(&root.pk, &root_key) == 0);
    assert(mbedtls_pk_check_pair(&host.pk, &host_key) == 0);

    /* The device certificate embeds the phone's public key unchanged. */
    static mbedtls_pk_context input;
    mbedtls_pk_init(&input);
    assert(mbedtls_pk_parse_public_key(
               &input, record.device_public_key,
               record.device_public_key_length + 1U) == 0);
    mbedtls_mpi input_modulus;
    mbedtls_mpi input_exponent;
    mbedtls_mpi embedded_modulus;
    mbedtls_mpi embedded_exponent;
    mbedtls_mpi_init(&input_modulus);
    mbedtls_mpi_init(&input_exponent);
    mbedtls_mpi_init(&embedded_modulus);
    mbedtls_mpi_init(&embedded_exponent);
    assert(mbedtls_rsa_export(mbedtls_pk_rsa(input), &input_modulus, NULL,
                              NULL, NULL, &input_exponent) == 0);
    assert(mbedtls_rsa_export(mbedtls_pk_rsa(device.pk), &embedded_modulus,
                              NULL, NULL, NULL, &embedded_exponent) == 0);
    assert(mbedtls_mpi_cmp_mpi(&input_modulus, &embedded_modulus) == 0);
    assert(mbedtls_mpi_cmp_mpi(&input_exponent, &embedded_exponent) == 0);

    mbedtls_mpi_free(&embedded_exponent);
    mbedtls_mpi_free(&embedded_modulus);
    mbedtls_mpi_free(&input_exponent);
    mbedtls_mpi_free(&input_modulus);
    mbedtls_pk_free(&input);
    mbedtls_pk_free(&host_key);
    mbedtls_pk_free(&root_key);
    mbedtls_x509_crt_free(&device);
    mbedtls_x509_crt_free(&host);
    mbedtls_x509_crt_free(&root);
}

static void test_generate_errors(void)
{
    static rtcp_pair_record_t record;
    char long_address[32];
    memset(long_address, 'a', sizeof(long_address) - 1U);
    long_address[sizeof(long_address) - 1U] = '\0';

    assert(rtcp_pair_record_generate(NULL, (const uint8_t *)"x", 1U, "w", "h",
                                     "s", test_rng, NULL) == RTCP_EINVAL);
    assert(rtcp_pair_record_generate(
               &record, (const uint8_t *)device_public_key_pem,
               sizeof(device_public_key_pem) - 1U, long_address, "h", "s",
               test_rng, NULL) == RTCP_EINVAL);
    assert(rtcp_pair_record_generate(
               &record, (const uint8_t *)device_public_key_pem,
               sizeof(device_public_key_pem) - 1U, "w", "h", "s", NULL,
               NULL) == RTCP_EINVAL);

    /* An unparsable device key fails before any key generation. */
    static const uint8_t not_a_key[] =
        "-----BEGIN RSA PUBLIC KEY-----\nAAAA\n";
    assert(rtcp_pair_record_generate(&record, not_a_key,
                                     sizeof(not_a_key) - 1U, "w", "h", "s",
                                     test_rng, NULL) == RTCP_EIO);

    /* A failing RNG aborts key generation and leaves no usable record. */
    assert(rtcp_pair_record_generate(
               &record, (const uint8_t *)device_public_key_pem,
               sizeof(device_public_key_pem) - 1U, "w", "h", "s", failing_rng,
               NULL) == RTCP_EIO);
    assert(record.root_certificate_length == 0U);
    assert(record.host_certificate_length == 0U);
}

int main(void)
{
    test_uuid();
    test_generate_and_verify();
    test_generate_errors();
    puts("lockdown pair host test passed");
    return 0;
}
