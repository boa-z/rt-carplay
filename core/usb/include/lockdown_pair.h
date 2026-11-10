/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Wired lockdown pair record.
 *
 * The USB pairing flow is not AirPlay Pair-Setup: the accessory generates a
 * local root CA, a host key and a certificate for the phone's device public
 * key, sends all three certificates (never a private key) in the Pair
 * request, and later uses the root identity as the TLS client certificate.
 * This matches the reference wired implementation and requires no Apple
 * identity material or private key extraction.
 *
 * Keys are RSA-2048 with SHA-256 signatures and empty distinguished names,
 * exactly like the reference.  Generation is CPU-heavy on the board, so call
 * it once and keep the record for the session.
 */
#ifndef RTCP_LOCKDOWN_PAIR_H
#define RTCP_LOCKDOWN_PAIR_H

#include <stddef.h>
#include <stdint.h>

#include "rtcp/rtcp.h"

#define RTCP_PAIR_UUID_CHARS 37U
#define RTCP_PAIR_DEVICE_KEY_MAX 2048U
#define RTCP_PAIR_CERTIFICATE_MAX 2048U
#define RTCP_PAIR_PRIVATE_KEY_MAX 4096U

typedef struct {
    char host_id[RTCP_PAIR_UUID_CHARS];
    char system_buid[RTCP_PAIR_UUID_CHARS];
    char wifi_address[24];

    /* PKCS#1 RSA public key PEM exactly as the phone reported it. */
    uint8_t device_public_key[RTCP_PAIR_DEVICE_KEY_MAX];
    size_t device_public_key_length;
    /* Self-signed by the local root over the phone's device key. */
    uint8_t device_certificate[RTCP_PAIR_CERTIFICATE_MAX];
    size_t device_certificate_length;

    /* Host key and certificate: part of the pair record, not the TLS
     * identity. */
    uint8_t host_private_key[RTCP_PAIR_PRIVATE_KEY_MAX];
    size_t host_private_key_length;
    uint8_t host_certificate[RTCP_PAIR_CERTIFICATE_MAX];
    size_t host_certificate_length;

    /* Root identity: the TLS client certificate and key. */
    uint8_t root_private_key[RTCP_PAIR_PRIVATE_KEY_MAX];
    size_t root_private_key_length;
    uint8_t root_certificate[RTCP_PAIR_CERTIFICATE_MAX];
    size_t root_certificate_length;
} rtcp_pair_record_t;

/* RNG callback: fill `length` bytes and return 0, or nonzero on failure.
 * The board passes a wrapper over its TRNG. */
typedef int (*rtcp_pair_rng_fn)(void *context, uint8_t *output, size_t length);

/* Generate a random RFC 4122 version 4 UUID string (36 characters plus NUL).
 * Returns RTCP_OK or a negative error. */
int rtcp_pair_generate_uuid(char *output, size_t capacity,
                            rtcp_pair_rng_fn rng, void *rng_context);

/* Fill `record` from the phone's device public key (PKCS#1 "RSA PUBLIC KEY"
 * PEM, as returned by lockdown GetValue DevicePublicKey), the Wi-Fi address
 * reported by the phone, and the caller-chosen host identity.  Generates the
 * host and root RSA keys and all three certificates.
 *
 * Returns RTCP_OK, RTCP_ENOMEM when a PEM does not fit, or RTCP_EIO when the
 * crypto library fails. */
int rtcp_pair_record_generate(rtcp_pair_record_t *record,
                              const uint8_t *device_public_key,
                              size_t device_public_key_length,
                              const char *wifi_address, const char *host_id,
                              const char *system_buid, rtcp_pair_rng_fn rng,
                              void *rng_context);

#endif /* RTCP_LOCKDOWN_PAIR_H */
