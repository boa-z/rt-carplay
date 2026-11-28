/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtcp/mfi_software.h"
#include "rtcp/rtcp.h"

#ifdef RTCP_MFI_SOFTWARE

#include <string.h>

#include "mbedtls/asn1.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/x509_crt.h"

#define DER_SEQUENCE          (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)
#define DER_SET               (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET)
#define DER_EXPLICIT          (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC)
#define OID_PKCS7_DATA        MBEDTLS_OID_PKCS "\x07\x01"
#define OID_PKCS7_SIGNED_DATA MBEDTLS_OID_PKCS "\x07\x02"
#define P256_COMPONENT_BYTES  32U

struct rtcp_mfi_software
{
    rtcp_mfi_provider_t provider;
    rtcp_mfi_provider_info_t info;
    const struct rtcp_os *os;
    int (*random)(void *, uint8_t *, size_t);
    void *random_context;
    rtcp_mutex_t lock;
    mbedtls_pk_context key;
    size_t signature_len;
    uint8_t certificate[];
};

static const unsigned char *der_content(unsigned char **cursor,
                                        const unsigned char *end, int tag)
{
    size_t len;
    if (mbedtls_asn1_get_tag(cursor, end, &len, tag) != 0)
        return NULL;
    return *cursor + len;
}

static bool read_oid(unsigned char **cursor, const unsigned char *end,
                     const char *value, size_t len)
{
    const unsigned char *next = der_content(cursor, end, MBEDTLS_ASN1_OID);
    if (next == NULL || (size_t)(next - *cursor) != len ||
        memcmp(*cursor, value, len) != 0)
        return false;
    *cursor += len;
    return true;
}

static bool read_certificate_range(unsigned char **cursor, const unsigned char **end)
{
    unsigned char *object = *cursor;
    if (der_content(cursor, *end, DER_SEQUENCE) != *end || *cursor == *end)
        return false;
    if (**cursor == DER_SEQUENCE)
    {
        *cursor = object;
        return true;
    }

    if (!read_oid(cursor, *end, OID_PKCS7_SIGNED_DATA, sizeof(OID_PKCS7_SIGNED_DATA) - 1U) ||
        der_content(cursor, *end, DER_EXPLICIT) != *end ||
        der_content(cursor, *end, DER_SEQUENCE) != *end)
        return false;

    int version;
    const unsigned char *next;
    if (mbedtls_asn1_get_int(cursor, *end, &version) != 0 || version != 1 ||
        (next = der_content(cursor, *end, DER_SET)) == NULL || next != *cursor ||
        (next = der_content(cursor, *end, DER_SEQUENCE)) == NULL ||
        !read_oid(cursor, next, OID_PKCS7_DATA, sizeof(OID_PKCS7_DATA) - 1U) ||
        next != *cursor)
        return false;

    const unsigned char *certificate_end = der_content(cursor, *end, DER_EXPLICIT);
    if (certificate_end == NULL || certificate_end == *cursor)
        return false;
    unsigned char *tail = *cursor + (certificate_end - *cursor);
    if (der_content(&tail, *end, DER_SET) != *end || tail != *end)
        return false;
    *end = certificate_end;
    return true;
}

static bool certificate_matches(const uint8_t *der, size_t len,
                                const mbedtls_pk_context *key)
{
    unsigned char *cursor = (unsigned char *)der;
    const unsigned char *end = der + len;
    if (!read_certificate_range(&cursor, &end))
        return false;

    size_t matching_leaves = 0U;
    while (cursor < end)
    {
        unsigned char *start = cursor;
        const unsigned char *next = der_content(&cursor, end, DER_SEQUENCE);
        if (next == NULL)
            return false;
        size_t cert_len = (size_t)(next - start);
        mbedtls_x509_crt cert;
        mbedtls_x509_crt_init(&cert);
        bool valid = mbedtls_x509_crt_parse_der_nocopy(&cert, start, cert_len) == 0 &&
                     cert.raw.len == cert_len;
        if (valid && cert.ca_istrue == 0 &&
            ((cert.ext_types & MBEDTLS_X509_EXT_KEY_USAGE) == 0 ||
             (cert.key_usage & MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0U) &&
            mbedtls_pk_check_pair(&cert.pk, key) == 0)
            ++matching_leaves;
        mbedtls_x509_crt_free(&cert);
        if (!valid)
            return false;
        cursor += next - cursor;
    }
    return matching_leaves == 1U;
}

static int provider_random(void *context, unsigned char *output, size_t len)
{
    const rtcp_mfi_software_t *software = context;
    if (software->random != NULL)
        return software->random(software->random_context, output, len);
    return software->os->random(output, len) ? 0 : -1;
}

static int provider_info(void *context, rtcp_mfi_provider_info_t *out)
{
    const rtcp_mfi_software_t *software = context;
    if (out == NULL)
        return RTCP_EINVAL;
    *out = software->info;
    return RTCP_OK;
}

static int provider_certificate(void *context, uint8_t *der, size_t cap, size_t *len)
{
    const rtcp_mfi_software_t *software = context;
    if (len == NULL)
        return RTCP_EINVAL;
    *len = 0U;
    if (der == NULL)
        return RTCP_EINVAL;
    if (cap < software->info.certificate_len)
        return RTCP_ENOMEM;
    memcpy(der, software->certificate, software->info.certificate_len);
    *len = software->info.certificate_len;
    return RTCP_OK;
}

static int sign_p256(rtcp_mfi_software_t *software, const uint8_t *digest,
                     size_t digest_len, uint8_t *signature)
{
    mbedtls_ecp_keypair *key = mbedtls_pk_ec(software->key);
    mbedtls_mpi first;
    mbedtls_mpi second;
    mbedtls_mpi_init(&first);
    mbedtls_mpi_init(&second);
    int result = mbedtls_ecdsa_sign(&key->grp, &first, &second, &key->d,
                                    digest, digest_len, provider_random, software);
    if (result == 0)
        result = mbedtls_mpi_write_binary(&first, signature, P256_COMPONENT_BYTES);
    if (result == 0)
        result = mbedtls_mpi_write_binary(&second, signature + P256_COMPONENT_BYTES,
                                          P256_COMPONENT_BYTES);
    mbedtls_mpi_free(&first);
    mbedtls_mpi_free(&second);
    return result;
}

static int provider_sign(void *context, const uint8_t *digest, size_t digest_len,
                         uint8_t *signature, size_t cap, size_t *len)
{
    rtcp_mfi_software_t *software = context;
    bool rsa = software->info.protocol_major == 2U;
    if (len == NULL)
        return RTCP_EINVAL;
    *len = 0U;
    if (digest == NULL || signature == NULL ||
        digest_len != (rsa ? RTCP_MFI_CHALLENGE_LEN : P256_COMPONENT_BYTES))
        return RTCP_EINVAL;
    if (cap < software->signature_len)
        return RTCP_ENOMEM;

    software->os->mutex_lock(software->lock);
    int result;
    if (rsa)
    {
        result = mbedtls_rsa_rsassa_pkcs1_v15_sign(
            mbedtls_pk_rsa(software->key), provider_random, software, MBEDTLS_RSA_PRIVATE,
            MBEDTLS_MD_SHA1, (unsigned int)digest_len, digest, signature);
    }
    else
    {
        result = sign_p256(software, digest, digest_len, signature);
    }
    software->os->mutex_unlock(software->lock);
    if (result != 0)
    {
        mbedtls_platform_zeroize(signature, software->signature_len);
        return RTCP_EIO;
    }
    *len = software->signature_len;
    return RTCP_OK;
}

static uint8_t supported_key(const mbedtls_pk_context *key, size_t *signature_len)
{
    if (mbedtls_pk_get_type(key) == MBEDTLS_PK_RSA)
    {
        size_t bits = mbedtls_pk_get_bitlen(key);
        if (bits >= 1024U && bits <= 2048U)
        {
            *signature_len = mbedtls_pk_get_len(key);
            return 2U;
        }
    }
    else if (mbedtls_pk_get_type(key) == MBEDTLS_PK_ECKEY &&
             mbedtls_pk_ec(*key)->grp.id == MBEDTLS_ECP_DP_SECP256R1)
    {
        *signature_len = 2U * P256_COMPONENT_BYTES;
        return 3U;
    }
    return 0U;
}

int rtcp_mfi_software_create(const rtcp_mfi_software_config_t *config,
                             rtcp_mfi_software_t **out)
{
    if (out == NULL)
        return RTCP_EINVAL;
    *out = NULL;
    if (config == NULL || config->os == NULL ||
        config->os->alloc == NULL || config->os->free == NULL ||
        config->os->mutex_create == NULL || config->os->mutex_destroy == NULL ||
        config->os->mutex_lock == NULL || config->os->mutex_unlock == NULL ||
        (config->random == NULL && config->os->random == NULL) ||
        config->certificate == NULL || config->certificate_len == 0U ||
        config->certificate_len > RTCP_MFI_WIRE_CERT_MAX ||
        config->private_key == NULL || config->private_key_len == 0U ||
        config->private_key_len > RTCP_MFI_SOFTWARE_KEY_MAX)
        return RTCP_EINVAL;

    rtcp_mfi_software_t *software = config->os->alloc(sizeof(*software) +
                                                      config->certificate_len);
    if (software == NULL)
        return RTCP_ENOMEM;
    memset(software, 0, sizeof(*software));
    software->os = config->os;
    software->random = config->random;
    software->random_context = config->random_context;
    mbedtls_pk_init(&software->key);
    int result = RTCP_EINVAL;
    if (mbedtls_pk_parse_key(&software->key, config->private_key, config->private_key_len,
                             NULL, 0U) != 0)
        goto failed;
    result = RTCP_ENOTSUP;
    uint8_t major = supported_key(&software->key, &software->signature_len);
    if (major == 0U || (config->protocol_major != 0U && config->protocol_major != major))
        goto failed;
    result = RTCP_EINVAL;
    if (!certificate_matches(config->certificate, config->certificate_len, &software->key))
        goto failed;
    result = RTCP_ENOMEM;
    software->lock = software->os->mutex_create();
    if (software->lock == NULL)
        goto failed;

    software->info = (rtcp_mfi_provider_info_t){
        .kind = RTCP_MFI_PROVIDER_SOFTWARE,
        .capabilities = RTCP_MFI_CAP_IAP2 | RTCP_MFI_CAP_SAP,
        .protocol_major = major,
        .certificate_len = config->certificate_len,
    };
    software->provider = (rtcp_mfi_provider_t){
        .context = software,
        .get_info = provider_info,
        .sign = provider_sign,
        .read_cert = provider_certificate,
    };
    memcpy(software->certificate, config->certificate, config->certificate_len);
    *out = software;
    return RTCP_OK;

failed:
    rtcp_mfi_software_destroy(software);
    return result;
}

const rtcp_mfi_provider_t *rtcp_mfi_software_provider(rtcp_mfi_software_t *software)
{
    return software != NULL ? &software->provider : NULL;
}

void rtcp_mfi_software_destroy(rtcp_mfi_software_t *software)
{
    if (software == NULL)
        return;
    const struct rtcp_os *os = software->os;
    if (software->lock != NULL)
        os->mutex_destroy(software->lock);
    mbedtls_pk_free(&software->key);
    mbedtls_platform_zeroize(software, sizeof(*software));
    os->free(software);
}

#else

int rtcp_mfi_software_create(const rtcp_mfi_software_config_t *config,
                             rtcp_mfi_software_t **out)
{
    (void)config;
    if (out == NULL)
        return RTCP_EINVAL;
    *out = NULL;
    return RTCP_ENOTSUP;
}

const rtcp_mfi_provider_t *rtcp_mfi_software_provider(rtcp_mfi_software_t *software)
{
    (void)software;
    return NULL;
}

void rtcp_mfi_software_destroy(rtcp_mfi_software_t *software)
{
    (void)software;
}

#endif
