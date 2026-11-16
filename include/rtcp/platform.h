/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_PLATFORM_H
#define RTCP_PLATFORM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rtcp/os.h"
#include "rtcp/net.h"

#define RTCP_CAPABILITIES_MAX_BYTES    6144U
#define RTCP_IDENTITY_PUBLIC_KEY_BYTES 32U
#define RTCP_IDENTITY_SIGNATURE_BYTES  64U

/* Identity and controller-store operations. All callbacks run on the
 * receiver owner. Credentials and contexts remain borrowed until destroy. */
typedef bool (*rtcp_identity_random_cb_t)(uint8_t *, size_t, void *);
typedef bool (*rtcp_identity_sign_cb_t)(const uint8_t *, size_t, uint8_t[64], void *);
typedef bool (*rtcp_identity_verify_cb_t)(const uint8_t[32], const uint8_t *,
                                          size_t, const uint8_t[64], void *);
typedef bool (*rtcp_pairing_save_cb_t)(const uint8_t *, size_t, const uint8_t[32], void *);
typedef bool (*rtcp_pairing_lookup_cb_t)(const uint8_t *, size_t, uint8_t[32], void *);
typedef struct
{
    const char *pairing_code;
    const uint8_t *accessory_identifier;
    size_t accessory_identifier_len;
    const uint8_t *accessory_public_key;
    rtcp_identity_random_cb_t random;
    rtcp_identity_sign_cb_t sign_accessory;
    rtcp_identity_verify_cb_t verify_controller;
    rtcp_pairing_save_cb_t save_pairing;
    void *provider_context;
} rtcp_pair_setup_provider_t;
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
typedef struct
{
    rtcp_pair_setup_provider_t pair_setup;
    rtcp_pair_verify_provider_t pair_verify;
} rtcp_pairing_provider_t;

typedef struct
{
    const struct rtcp_os *os;
    const struct rtcp_net *net;
    rtcp_pairing_provider_t pairing;
    /* Initial binary /info capability document: copied at create. A refresh
     * callback runs before each accepted session; bytes are copied before
     * callback-owned storage may change. Failure rejects that connection. */
    const uint8_t *info_plist;
    size_t info_plist_len;
    bool (*get_info_plist)(const uint8_t **bytes, size_t *length, void *context);
    void *info_context;
} rtcp_platform_t;

/* Optional authenticated iAP2 tunnel. The host owns radio/Wi-Fi policy.
 * A false tick retires only the tunnel, preserving the media session. */
typedef struct
{
    bool (*on_bytes)(const uint8_t *, size_t, void *);
    bool (*on_tick)(void *);
    void (*on_stop)(void *);
    bool (*disable_bluetooth)(const char *device_id, void *);
    void *context;
} rtcp_iap2_backend_t;
#endif
