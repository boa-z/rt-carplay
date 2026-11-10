/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lockdown_pair.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"

#define DER_ARENA_BYTES 8192U
#define RSA_KEY_BITS 2048
#define RSA_PUBLIC_EXPONENT 65537

/* Fixed validity covering the bring-up period.  Lockdown pair records are
 * self-signed and the phone does not use them for PKI; using a constant keeps
 * the core free of a wall clock. */
#define CERT_NOT_BEFORE "250101000000Z"
#define CERT_NOT_BEFORE_LENGTH 13U
#define CERT_NOT_AFTER "350101000000Z"
#define CERT_NOT_AFTER_LENGTH 13U

typedef struct {
    uint8_t bytes[DER_ARENA_BYTES];
    size_t used;
} der_arena_t;

typedef struct {
    const uint8_t *data;
    size_t length;
} der_node_t;

typedef struct {
    rtcp_pair_rng_fn function;
    void *context;
} rng_bridge_t;

static void arena_reset(der_arena_t *arena)
{
    arena->used = 0U;
}

static bool arena_claim(der_arena_t *arena, size_t length, uint8_t **result)
{
    if (length > sizeof(arena->bytes) - arena->used) {
        return false;
    }
    *result = arena->bytes + arena->used;
    arena->used += length;
    return true;
}

static size_t length_header_size(size_t content)
{
    if (content < 0x80U) {
        return 2U;
    }
    size_t count = 0U;
    for (size_t value = content; value != 0U; value >>= 8) {
        ++count;
    }
    return 2U + count;
}

static size_t write_header(uint8_t *output, uint8_t tag, size_t content)
{
    output[0] = tag;
    if (content < 0x80U) {
        output[1] = (uint8_t)content;
        return 2U;
    }
    size_t count = 0U;
    for (size_t value = content; value != 0U; value >>= 8) {
        ++count;
    }
    output[1] = (uint8_t)(0x80U | count);
    for (size_t i = 0U; i < count; ++i) {
        output[2U + i] = (uint8_t)(content >> (8U * (count - 1U - i)));
    }
    return 2U + count;
}

static bool der_wrap(der_arena_t *arena, uint8_t tag, const uint8_t *content,
                     size_t content_length, der_node_t *node)
{
    size_t header = length_header_size(content_length);
    uint8_t *output = NULL;
    if (!arena_claim(arena, header + content_length, &output)) {
        return false;
    }
    header = write_header(output, tag, content_length);
    if (content_length != 0U) {
        memmove(output + header, content, content_length);
    }
    node->data = output;
    node->length = header + content_length;
    return true;
}

static bool der_wrap_parts(der_arena_t *arena, uint8_t tag,
                           const der_node_t *parts, size_t count,
                           der_node_t *node)
{
    size_t content_length = 0U;
    for (size_t i = 0U; i < count; ++i) {
        if (parts[i].length > SIZE_MAX - content_length) {
            return false;
        }
        content_length += parts[i].length;
    }
    size_t header = length_header_size(content_length);
    uint8_t *output = NULL;
    if (!arena_claim(arena, header + content_length, &output)) {
        return false;
    }
    header = write_header(output, tag, content_length);
    size_t offset = header;
    for (size_t i = 0U; i < count; ++i) {
        memcpy(output + offset, parts[i].data, parts[i].length);
        offset += parts[i].length;
    }
    node->data = output;
    node->length = offset;
    return true;
}

static bool der_integer_positive(der_arena_t *arena, const uint8_t *bytes,
                                 size_t length, der_node_t *node)
{
    size_t offset = 0U;
    while (offset + 1U < length && bytes[offset] == 0U) {
        ++offset;
    }
    size_t content_length = length - offset;
    bool pad = (bytes[offset] & 0x80U) != 0U;
    size_t header = length_header_size(content_length + (pad ? 1U : 0U));
    uint8_t *output = NULL;
    if (!arena_claim(arena, header + content_length + (pad ? 1U : 0U),
                     &output)) {
        return false;
    }
    header = write_header(output, 0x02U,
                          content_length + (pad ? 1U : 0U));
    if (pad) {
        output[header] = 0U;
        ++header;
    }
    memcpy(output + header, bytes + offset, content_length);
    node->data = output;
    node->length = header + content_length;
    return true;
}

static bool der_integer_mpi(der_arena_t *arena, const mbedtls_mpi *value,
                            der_node_t *node)
{
    size_t length = mbedtls_mpi_size(value);
    if (length == 0U || length > RSA_KEY_BITS / 8 + 1U) {
        return false;
    }
    uint8_t bytes[RSA_KEY_BITS / 8 + 1U];
    if (mbedtls_mpi_write_binary(value, bytes, length) != 0) {
        return false;
    }
    return der_integer_positive(arena, bytes, length, node);
}

static bool der_small_integer(der_arena_t *arena, uint8_t value,
                              der_node_t *node)
{
    return der_integer_positive(arena, &value, 1U, node);
}

static bool der_boolean(der_arena_t *arena, bool value, der_node_t *node)
{
    uint8_t content = value ? 0xffU : 0x00U;
    return der_wrap(arena, 0x01U, &content, 1U, node);
}

static bool der_empty_sequence(der_arena_t *arena, der_node_t *node)
{
    return der_wrap_parts(arena, 0x30U, NULL, 0U, node);
}

static bool der_sequence(der_arena_t *arena, const der_node_t *parts,
                         size_t count, der_node_t *node)
{
    return der_wrap_parts(arena, 0x30U, parts, count, node);
}

static bool der_octet_string(der_arena_t *arena, const der_node_t *inner,
                             der_node_t *node)
{
    return der_wrap(arena, 0x04U, inner->data, inner->length, node);
}

static bool der_bit_string(der_arena_t *arena, uint8_t unused,
                           const uint8_t *bits, size_t length,
                           der_node_t *node)
{
    size_t content_length = 1U + length;
    size_t header = length_header_size(content_length);
    uint8_t *output = NULL;
    if (!arena_claim(arena, header + content_length, &output)) {
        return false;
    }
    header = write_header(output, 0x03U, content_length);
    output[header] = unused;
    memcpy(output + header + 1U, bits, length);
    node->data = output;
    node->length = header + content_length;
    return true;
}

static bool der_oid(der_arena_t *arena, const uint8_t *content,
                    size_t length, der_node_t *node)
{
    return der_wrap(arena, 0x06U, content, length, node);
}

static bool der_utc_time(der_arena_t *arena, const char *time,
                         size_t length, der_node_t *node)
{
    return der_wrap(arena, 0x17U, (const uint8_t *)time, length, node);
}

static bool der_explicit(der_arena_t *arena, uint8_t number,
                         const der_node_t *inner, der_node_t *node)
{
    return der_wrap(arena, (uint8_t)(0xa0U + number), inner->data,
                    inner->length, node);
}

static bool der_null(der_arena_t *arena, der_node_t *node)
{
    return der_wrap(arena, 0x05U, NULL, 0U, node);
}

/* mbedTLS rejects an empty issuer Name (the reference implementation emits an
 * empty sequence, which JCA tolerates), so all certificates carry a local
 * common name.  It is not an Apple identity and is not checked by lockdown. */
static bool der_name(der_arena_t *arena, der_node_t *node)
{
    static const uint8_t common_name_oid[] = {0x55, 0x04, 0x03};
    static const char common_name[] = "rt-carplay";
    der_node_t oid;
    der_node_t value;
    if (!der_oid(arena, common_name_oid, sizeof(common_name_oid), &oid) ||
        !der_wrap(arena, 0x0cU, (const uint8_t *)common_name,
                  sizeof(common_name) - 1U, &value)) {
        return false;
    }
    der_node_t attribute_parts[2] = {oid, value};
    der_node_t attribute;
    der_node_t set;
    if (!der_sequence(arena, attribute_parts, 2U, &attribute) ||
        !der_wrap(arena, 0x31U, attribute.data, attribute.length, &set)) {
        return false;
    }
    return der_sequence(arena, &set, 1U, node);
}

static int rng_bridge(void *context, unsigned char *output, size_t length)
{
    rng_bridge_t *bridge = context;
    return bridge->function(bridge->context, output, length);
}

static bool algorithm_sha256_rsa(der_arena_t *arena, der_node_t *node)
{
    static const uint8_t oid[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                  0x0d, 0x01, 0x01, 0x0b};
    der_node_t parts[2];
    if (!der_oid(arena, oid, sizeof(oid), &parts[0]) ||
        !der_null(arena, &parts[1])) {
        return false;
    }
    return der_sequence(arena, parts, 2U, node);
}

static bool algorithm_rsa(der_arena_t *arena, der_node_t *node)
{
    static const uint8_t oid[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                  0x0d, 0x01, 0x01, 0x01};
    der_node_t parts[2];
    if (!der_oid(arena, oid, sizeof(oid), &parts[0]) ||
        !der_null(arena, &parts[1])) {
        return false;
    }
    return der_sequence(arena, parts, 2U, node);
}

static bool subject_public_key_info(der_arena_t *arena,
                                    const mbedtls_mpi *modulus,
                                    const mbedtls_mpi *exponent,
                                    der_node_t *spki, der_node_t *pkcs1_out)
{
    der_node_t values[2];
    der_node_t pkcs1;
    if (!der_integer_mpi(arena, modulus, &values[0]) ||
        !der_integer_mpi(arena, exponent, &values[1]) ||
        !der_sequence(arena, values, 2U, &pkcs1)) {
        return false;
    }
    der_node_t algorithm;
    der_node_t bit_string;
    if (!algorithm_rsa(arena, &algorithm) ||
        !der_bit_string(arena, 0U, pkcs1.data, pkcs1.length, &bit_string)) {
        return false;
    }
    der_node_t parts[2] = {algorithm, bit_string};
    if (!der_sequence(arena, parts, 2U, spki)) {
        return false;
    }
    if (pkcs1_out != NULL) {
        *pkcs1_out = pkcs1;
    }
    return true;
}

static bool extension(der_arena_t *arena, const uint8_t *oid,
                      size_t oid_length, bool critical,
                      const der_node_t *value, der_node_t *node)
{
    der_node_t parts[3];
    size_t count = 0U;
    if (!der_oid(arena, oid, oid_length, &parts[count++])) {
        return false;
    }
    if (critical) {
        if (!der_boolean(arena, true, &parts[count++])) {
            return false;
        }
    }
    der_node_t encoded;
    if (!der_octet_string(arena, value, &encoded)) {
        return false;
    }
    parts[count++] = encoded;
    return der_sequence(arena, parts, count, node);
}

static bool extensions_root(der_arena_t *arena, der_node_t *node)
{
    static const uint8_t basic_constraints[] = {0x55, 0x1d, 0x13};
    der_node_t critical_value;
    der_node_t critical_seq;
    if (!der_boolean(arena, true, &critical_value) ||
        !der_sequence(arena, &critical_value, 1U, &critical_seq)) {
        return false;
    }
    der_node_t item;
    if (!extension(arena, basic_constraints, sizeof(basic_constraints), true,
                   &critical_seq, &item)) {
        return false;
    }
    return der_sequence(arena, &item, 1U, node);
}

static bool extensions_leaf(der_arena_t *arena, const der_node_t *pkcs1,
                            bool include_subject_key_id, der_node_t *node)
{
    static const uint8_t basic_constraints[] = {0x55, 0x1d, 0x13};
    static const uint8_t subject_key_id[] = {0x55, 0x1d, 0x0e};
    static const uint8_t key_usage[] = {0x55, 0x1d, 0x0f};
    static const uint8_t usage_bits[] = {0xa0};
    der_node_t items[4];
    size_t count = 0U;

    der_node_t empty;
    if (!der_empty_sequence(arena, &empty) ||
        !extension(arena, basic_constraints, sizeof(basic_constraints), true,
                   &empty, &items[count++])) {
        return false;
    }

    if (include_subject_key_id) {
        uint8_t digest[20];
        if (mbedtls_sha1_ret(pkcs1->data, pkcs1->length, digest) != 0) {
            return false;
        }
        der_node_t digest_value;
        if (!der_wrap(arena, 0x04U, digest, sizeof(digest), &digest_value) ||
            !extension(arena, subject_key_id, sizeof(subject_key_id), false,
                       &digest_value, &items[count])) {
            return false;
        }
        ++count;
    }

    der_node_t usage;
    if (!der_bit_string(arena, 5U, usage_bits, sizeof(usage_bits), &usage) ||
        !extension(arena, key_usage, sizeof(key_usage), true, &usage,
                   &items[count++])) {
        return false;
    }
    return der_sequence(arena, items, count, node);
}

static bool build_certificate(der_arena_t *arena, mbedtls_pk_context *signer,
                              const mbedtls_mpi *subject_modulus,
                              const mbedtls_mpi *subject_exponent,
                              bool is_root, bool include_subject_key_id,
                              der_node_t *certificate)
{
    arena_reset(arena);

    der_node_t version_value;
    der_node_t version;
    der_node_t serial;
    der_node_t algorithm;
    der_node_t issuer;
    der_node_t subject;
    der_node_t spki;
    der_node_t pkcs1;
    der_node_t not_before;
    der_node_t not_after;
    der_node_t validity_parts[2];
    der_node_t validity;
    der_node_t extensions;
    der_node_t extensions_explicit;

    if (!der_small_integer(arena, 2U, &version_value) ||
        !der_explicit(arena, 0U, &version_value, &version) ||
        !der_small_integer(arena, 0U, &serial) ||
        !algorithm_sha256_rsa(arena, &algorithm) ||
        !der_name(arena, &issuer) ||
        !der_name(arena, &subject) ||
        !subject_public_key_info(arena, subject_modulus, subject_exponent,
                                 &spki, &pkcs1) ||
        !der_utc_time(arena, CERT_NOT_BEFORE, CERT_NOT_BEFORE_LENGTH,
                      &not_before) ||
        !der_utc_time(arena, CERT_NOT_AFTER, CERT_NOT_AFTER_LENGTH,
                      &not_after)) {
        return false;
    }
    validity_parts[0] = not_before;
    validity_parts[1] = not_after;
    if (!der_sequence(arena, validity_parts, 2U, &validity)) {
        return false;
    }

    bool extensions_ok =
        is_root ? extensions_root(arena, &extensions)
                : extensions_leaf(arena, &pkcs1, include_subject_key_id,
                                  &extensions);
    if (!extensions_ok ||
        !der_explicit(arena, 3U, &extensions, &extensions_explicit)) {
        return false;
    }

    der_node_t tbs_parts[7] = {version, serial,   algorithm, issuer,
                               validity, subject,  spki};
    /* The extensions field follows the subject public key info. */
    der_node_t tbs_all[8];
    for (size_t i = 0U; i < 7U; ++i) {
        tbs_all[i] = tbs_parts[i];
    }
    tbs_all[7] = extensions_explicit;
    der_node_t tbs;
    if (!der_sequence(arena, tbs_all, 8U, &tbs)) {
        return false;
    }

    uint8_t digest[32];
    if (mbedtls_sha256_ret(tbs.data, tbs.length, digest, 0) != 0) {
        return false;
    }
    uint8_t signature[RSA_KEY_BITS / 8];
    size_t signature_length = 0U;
    if (mbedtls_pk_sign(signer, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                        signature, &signature_length, NULL, NULL) != 0 ||
        signature_length != sizeof(signature)) {
        return false;
    }
    der_node_t signature_bits;
    if (!der_bit_string(arena, 0U, signature, signature_length,
                        &signature_bits)) {
        return false;
    }
    der_node_t cert_parts[3] = {tbs, algorithm, signature_bits};
    return der_sequence(arena, cert_parts, 3U, certificate);
}

static int generate_key(mbedtls_pk_context *key, rng_bridge_t *rng)
{
    int ret = mbedtls_pk_setup(key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret != 0) {
        return ret;
    }
    return mbedtls_rsa_gen_key(mbedtls_pk_rsa(*key), rng_bridge, rng,
                               RSA_KEY_BITS, RSA_PUBLIC_EXPONENT);
}

static int export_public(const mbedtls_pk_context *key, mbedtls_mpi *modulus,
                         mbedtls_mpi *exponent)
{
    if (mbedtls_pk_get_type(key) != MBEDTLS_PK_RSA) {
        return -1;
    }
    return mbedtls_rsa_export(mbedtls_pk_rsa(*key), modulus, NULL, NULL, NULL,
                              exponent);
}

static int parse_device_public_key(const uint8_t *pem, size_t pem_length,
                                   mbedtls_mpi *modulus,
                                   mbedtls_mpi *exponent)
{
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    /* mbedtls_pk_parse_public_key() expects the trailing NUL in the length
     * for PEM input. */
    int ret = mbedtls_pk_parse_public_key(&key, pem, pem_length + 1U);
    if (ret == 0) {
        ret = export_public(&key, modulus, exponent);
    }
    mbedtls_pk_free(&key);
    return ret;
}

static int encode_pem(uint8_t *output, size_t capacity, const char *label,
                      const uint8_t *der, size_t der_length)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t label_length = strlen(label);
    size_t base64_length = ((der_length + 2U) / 3U) * 4U;
    size_t needed = 17U + label_length;            /* -----BEGIN label----- */
    needed += base64_length + (base64_length + 63U) / 64U;
    needed += 15U + label_length;                  /* -----END label----- */
    needed += 1U;                                  /* NUL */
    if (output == NULL || capacity < needed) {
        return RTCP_ENOMEM;
    }

    size_t offset = 0U;
    int written = snprintf((char *)output, capacity, "-----BEGIN %s-----\n",
                           label);
    if (written <= 0) {
        return RTCP_EIO;
    }
    offset = (size_t)written;

    size_t column = 0U;
    for (size_t i = 0U; i < der_length; i += 3U) {
        size_t remaining = der_length - i;
        uint32_t group = (uint32_t)der[i] << 16;
        if (remaining > 1U) {
            group |= (uint32_t)der[i + 1U] << 8;
        }
        if (remaining > 2U) {
            group |= der[i + 2U];
        }
        char quartet[4];
        quartet[0] = alphabet[(group >> 18) & 0x3fU];
        quartet[1] = alphabet[(group >> 12) & 0x3fU];
        quartet[2] = remaining > 1U ? alphabet[(group >> 6) & 0x3fU] : '=';
        quartet[3] = remaining > 2U ? alphabet[group & 0x3fU] : '=';
        for (size_t c = 0U; c < 4U; ++c) {
            output[offset++] = (uint8_t)quartet[c];
            if (++column == 64U) {
                output[offset++] = '\n';
                column = 0U;
            }
        }
    }
    if (column != 0U) {
        output[offset++] = '\n';
    }
    written = snprintf((char *)output + offset, capacity - offset,
                       "-----END %s-----\n", label);
    if (written <= 0) {
        return RTCP_EIO;
    }
    offset += (size_t)written;
    output[offset] = '\0';
    return RTCP_OK;
}

static int write_private_key(mbedtls_pk_context *key, uint8_t *output,
                             size_t capacity, size_t *length)
{
    if (mbedtls_pk_write_key_pem(key, output, capacity) != 0) {
        return RTCP_ENOMEM;
    }
    *length = strlen((const char *)output);
    return RTCP_OK;
}

int rtcp_pair_generate_uuid(char *output, size_t capacity,
                            rtcp_pair_rng_fn rng, void *rng_context)
{
    if (output == NULL || capacity < RTCP_PAIR_UUID_CHARS || rng == NULL) {
        return RTCP_EINVAL;
    }
    uint8_t bytes[16];
    if (rng(rng_context, bytes, sizeof(bytes)) != 0) {
        return RTCP_EIO;
    }
    bytes[6] = (uint8_t)((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = (uint8_t)((bytes[8] & 0x3fU) | 0x80U);
    static const char hex[] = "0123456789ABCDEF";
    size_t offset = 0U;
    for (size_t i = 0U; i < sizeof(bytes); ++i) {
        if (i == 4U || i == 6U || i == 8U || i == 10U) {
            output[offset++] = '-';
        }
        output[offset++] = hex[bytes[i] >> 4];
        output[offset++] = hex[bytes[i] & 0x0fU];
    }
    output[offset] = '\0';
    return RTCP_OK;
}

int rtcp_pair_record_generate(rtcp_pair_record_t *record,
                              const uint8_t *device_public_key,
                              size_t device_public_key_length,
                              const char *wifi_address, const char *host_id,
                              const char *system_buid, rtcp_pair_rng_fn rng,
                              void *rng_context)
{
    static der_arena_t arena;
    rng_bridge_t rng_bridge_value = {rng, rng_context};
    mbedtls_mpi device_modulus;
    mbedtls_mpi device_exponent;
    mbedtls_mpi host_modulus;
    mbedtls_mpi host_exponent;
    mbedtls_mpi root_modulus;
    mbedtls_mpi root_exponent;
    mbedtls_pk_context host_key;
    mbedtls_pk_context root_key;
    int result = RTCP_EIO;

    if (record == NULL || device_public_key == NULL ||
        device_public_key_length == 0U || wifi_address == NULL ||
        host_id == NULL || system_buid == NULL || rng == NULL ||
        strlen(wifi_address) >= sizeof(record->wifi_address) ||
        strlen(host_id) >= sizeof(record->host_id) ||
        strlen(system_buid) >= sizeof(record->system_buid) ||
        device_public_key_length + 1U > sizeof(record->device_public_key)) {
        return RTCP_EINVAL;
    }

    memset(record, 0, sizeof(*record));
    memcpy(record->host_id, host_id, strlen(host_id) + 1U);
    memcpy(record->system_buid, system_buid, strlen(system_buid) + 1U);
    memcpy(record->wifi_address, wifi_address, strlen(wifi_address) + 1U);
    memcpy(record->device_public_key, device_public_key,
           device_public_key_length);
    record->device_public_key[device_public_key_length] = '\0';
    record->device_public_key_length = device_public_key_length;

    mbedtls_mpi_init(&device_modulus);
    mbedtls_mpi_init(&device_exponent);
    mbedtls_mpi_init(&host_modulus);
    mbedtls_mpi_init(&host_exponent);
    mbedtls_mpi_init(&root_modulus);
    mbedtls_mpi_init(&root_exponent);
    mbedtls_pk_init(&host_key);
    mbedtls_pk_init(&root_key);

    if (parse_device_public_key(record->device_public_key,
                                record->device_public_key_length,
                                &device_modulus, &device_exponent) != 0) {
        goto cleanup;
    }
    if (generate_key(&host_key, &rng_bridge_value) != 0) {
        goto cleanup;
    }
    if (generate_key(&root_key, &rng_bridge_value) != 0) {
        goto cleanup;
    }
    if (export_public(&host_key, &host_modulus, &host_exponent) != 0 ||
        export_public(&root_key, &root_modulus, &root_exponent) != 0) {
        goto cleanup;
    }

    der_node_t certificate;
    if (!build_certificate(&arena, &root_key, &root_modulus, &root_exponent,
                           true, false, &certificate) ||
        encode_pem(record->root_certificate, sizeof(record->root_certificate),
                   "CERTIFICATE", certificate.data,
                   certificate.length) != RTCP_OK) {
        goto cleanup;
    }
    record->root_certificate_length =
        strlen((const char *)record->root_certificate);

    if (write_private_key(&root_key, record->root_private_key,
                          sizeof(record->root_private_key),
                          &record->root_private_key_length) != RTCP_OK) {
        goto cleanup;
    }

    if (!build_certificate(&arena, &root_key, &host_modulus, &host_exponent,
                           false, false, &certificate) ||
        encode_pem(record->host_certificate, sizeof(record->host_certificate),
                   "CERTIFICATE", certificate.data,
                   certificate.length) != RTCP_OK) {
        goto cleanup;
    }
    record->host_certificate_length =
        strlen((const char *)record->host_certificate);

    if (write_private_key(&host_key, record->host_private_key,
                          sizeof(record->host_private_key),
                          &record->host_private_key_length) != RTCP_OK) {
        goto cleanup;
    }

    if (!build_certificate(&arena, &root_key, &device_modulus,
                           &device_exponent, false, true, &certificate) ||
        encode_pem(record->device_certificate,
                   sizeof(record->device_certificate), "CERTIFICATE",
                   certificate.data, certificate.length) != RTCP_OK) {
        goto cleanup;
    }
    record->device_certificate_length =
        strlen((const char *)record->device_certificate);

    result = RTCP_OK;

cleanup:
    mbedtls_pk_free(&root_key);
    mbedtls_pk_free(&host_key);
    mbedtls_mpi_free(&root_exponent);
    mbedtls_mpi_free(&root_modulus);
    mbedtls_mpi_free(&host_exponent);
    mbedtls_mpi_free(&host_modulus);
    mbedtls_mpi_free(&device_exponent);
    mbedtls_mpi_free(&device_modulus);
    if (result != RTCP_OK) {
        memset(record, 0, sizeof(*record));
    }
    return result;
}
