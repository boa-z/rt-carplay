/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file platform.h
 * Receiver dependency injection and authentication provider contracts. */
#ifndef RTCP_PLATFORM_H
#define RTCP_PLATFORM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rtcp/mfi.h"
#include "rtcp/os.h"
#include "rtcp/net.h"

#define RTCP_CAPABILITIES_MAX_BYTES    6144U
#define RTCP_IDENTITY_PUBLIC_KEY_BYTES 32U
#define RTCP_IDENTITY_SIGNATURE_BYTES  64U

/** Fill the caller's output with cryptographically secure random bytes.
 * Return false if entropy is unavailable. Called on the Receiver owner. */
typedef bool (*rtcp_identity_random_cb_t)(uint8_t *, size_t, void *);
/** Sign a borrowed message with the accessory key. Fill the caller's
 * 64-byte output and return true, or fail authentication with false. */
typedef bool (*rtcp_identity_sign_cb_t)(const uint8_t *, size_t, uint8_t[64], void *);
/** Return true if the 64-byte signature matches the borrowed message and
 * 32-byte controller public key. Called on the Receiver owner. */
typedef bool (*rtcp_identity_verify_cb_t)(const uint8_t[32], const uint8_t *,
                                          size_t, const uint8_t[64], void *);
/** Persist a controller identifier and key before returning true. Both
 * inputs borrow request storage. False rejects pairing. */
typedef bool (*rtcp_pairing_save_cb_t)(const uint8_t *, size_t, const uint8_t[32], void *);
/** Copy the stored key for an identifier into the 32-byte output. Return
 * false when it cannot be supplied, which rejects Pair-Verify. */
typedef bool (*rtcp_pairing_lookup_cb_t)(const uint8_t *, size_t, uint8_t[32], void *);
/** Pair-Setup provider. Owner-thread callbacks borrow credentials and
 * provider_context through Receiver destruction. */
typedef struct
{
    const char *pairing_code; /**< NUL-terminated PIN retained by the host. */
    const uint8_t *accessory_identifier;
    size_t accessory_identifier_len;
    const uint8_t *accessory_public_key;
    rtcp_identity_random_cb_t random;
    rtcp_identity_sign_cb_t sign_accessory;
    rtcp_identity_verify_cb_t verify_controller;
    rtcp_pairing_save_cb_t save_pairing;
    void *provider_context;
} rtcp_pair_setup_provider_t;
/** Pair-Verify provider for previously established controller identities. */
typedef struct
{
    const uint8_t *accessory_identifier;
    size_t accessory_identifier_len;
    rtcp_identity_random_cb_t random;
    rtcp_identity_sign_cb_t sign_accessory;
    rtcp_identity_verify_cb_t verify_controller;
    rtcp_pairing_lookup_cb_t lookup_controller;
    void *provider_context;
} rtcp_pair_verify_provider_t;
/** Platform pairing operations for new and remembered controllers. */
typedef struct
{
    rtcp_pair_setup_provider_t pair_setup;
    rtcp_pair_verify_provider_t pair_verify;
} rtcp_pairing_provider_t;

/** Platform tables and /info provider. Tables and contexts remain borrowed
 * and valid until Receiver destruction. */
typedef struct
{
    const struct rtcp_os *os;
    const struct rtcp_net *net;
    rtcp_pairing_provider_t pairing;
    /** Initial binary /info capability document: copied at create. A refresh
     * callback runs before each accepted session; bytes are copied before
     * callback-owned storage may change. Failure rejects that connection. */
    const uint8_t *info_plist;
    size_t info_plist_len;
    /** Optionally supply immutable /info bytes on the owner before each
     * accepted session. Keep bytes valid until the Receiver copies them
     * immediately after the callback. False rejects that connection. */
    bool (*get_info_plist)(const uint8_t **bytes, size_t *length, void *context);
    void *info_context;
    /** Classic MFiSAP identity. Retain until Receiver destruction. A NULL
     * pointer keeps compatibility with the process-wide legacy SAP setter. */
    const rtcp_mfi_provider_t *mfi_provider;
} rtcp_platform_t;

/** Optional authenticated iAP2 tunnel. The host owns radio/Wi-Fi policy.
 * A false tick retires only the tunnel, preserving the media session. */
typedef struct
{
    /** Borrow authenticated stream bytes until return. False closes tunnel. */
    bool (*on_bytes)(const uint8_t *, size_t, void *);
    bool (*on_tick)(void *);
    /** Retire tunnel resources on the Receiver owner. */
    void (*on_stop)(void *);
    bool (*disable_bluetooth)(const char *device_id, void *);
    void *context;
} rtcp_iap2_backend_t;
#endif
