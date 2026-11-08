/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_mfi_sap.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

/* RFC 7748 test vector: Alice's X25519 public key. */
static const uint8_t PEER_PUBLIC_KEY[32] = {
    0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54,
    0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
    0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4,
    0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a,
};

static unsigned g_random_calls;

static int fake_random(uint8_t *out, size_t len, void *context)
{
    (void)context;
    ++g_random_calls;
    for (size_t i = 0; i < len; ++i) out[i] = (uint8_t)(0x40U + i);
    return RTCP_OK;
}

static int fake_read_cert(uint8_t *der, size_t cap, size_t *der_len)
{
    if (cap < 908U) return RTCP_EINVAL;
    memset(der, 0xc3, 908U);
    *der_len = 908U;
    return RTCP_OK;
}

static int fake_sign(const uint8_t *challenge, size_t len,
                     uint8_t *sig, size_t cap, size_t *sig_len)
{
    (void)challenge;
    if (len == 32U) return RTCP_EINVAL; /* no Auth 3.0 identity here */
    assert(len == 20U); /* MFi 2.0C: SHA-1 digest */
    if (cap < 128U) return RTCP_EINVAL;
    memset(sig, 0x5a, 128U);
    *sig_len = 128U;
    return RTCP_OK;
}

int main(void)
{
    static const struct rtcp_mfi mfi = {
        .read_cert = fake_read_cert,
        .sign = fake_sign,
    };
    uint8_t response[2048];
    size_t response_len = 99U;
    uint8_t request[33];
    request[0] = 1U;
    memcpy(request + 1, PEER_PUBLIC_KEY, sizeof(PEER_PUBLIC_KEY));

    /* No provider registered. */
    assert(airplay_mfi_sap_respond(request, sizeof(request), response,
                                   sizeof(response), &response_len) ==
           AIRPLAY_MFI_SAP_UNAVAILABLE);
    assert(response_len == 0U);

    airplay_mfi_sap_provider_t provider = {
        .mfi = &mfi,
        .protocol_major = 1U,
        .random = fake_random,
    };
    assert(!airplay_mfi_sap_set_provider(&provider)); /* 2.0C or 3.0 only */

    provider.protocol_major = 2U;
    provider.mfi = NULL;
    assert(!airplay_mfi_sap_set_provider(&provider));
    provider.mfi = &mfi;
    assert(airplay_mfi_sap_set_provider(&provider));

    assert(airplay_mfi_sap_respond(request, sizeof(request) - 1U, response,
                                   sizeof(response), &response_len) ==
           AIRPLAY_MFI_SAP_INVALID_ARGUMENT);
    request[0] = 2U;
    assert(airplay_mfi_sap_respond(request, sizeof(request), response,
                                   sizeof(response), &response_len) ==
           AIRPLAY_MFI_SAP_INVALID_ARGUMENT);
    request[0] = 1U;

    assert(airplay_mfi_sap_respond(request, sizeof(request), response,
                                   sizeof(response), &response_len) ==
           AIRPLAY_MFI_SAP_OK);
    assert(g_random_calls > 0U);
    assert(response_len == 32U + 4U + 908U + 4U + 128U);

    /* The accessory generated a fresh X25519 key, not an echo. */
    assert(memcmp(response, PEER_PUBLIC_KEY, 32U) != 0);
    uint8_t aggregate = 0U;
    for (size_t i = 0; i < 32U; ++i) aggregate |= response[i];
    assert(aggregate != 0U);

    assert(response[32] == 0 && response[33] == 0 &&
           response[34] == 0x03 && response[35] == 0x8c);
    for (size_t i = 0; i < 908U; ++i) assert(response[36U + i] == 0xc3U);
    size_t sig_len_off = 36U + 908U;
    assert(response[sig_len_off] == 0 && response[sig_len_off + 1] == 0 &&
           response[sig_len_off + 2] == 0 && response[sig_len_off + 3] == 128);

    /* The signature is AES-CTR encrypted, so it must not leak the raw bytes. */
    bool raw_signature_visible = true;
    for (size_t i = 0; i < 128U; ++i) {
        if (response[sig_len_off + 4U + i] != 0x5aU) raw_signature_visible = false;
    }
    assert(!raw_signature_visible);

    /* Buffer sizing is checked before any crypto work. */
    assert(airplay_mfi_sap_respond(request, sizeof(request), response,
                                   response_len - 1U, &response_len) ==
           AIRPLAY_MFI_SAP_BUFFER_TOO_SMALL);

    /* Protocol 3 uses SHA-256, which the 2.0C fake provider rejects. */
    provider.protocol_major = 3U;
    assert(airplay_mfi_sap_set_provider(&provider));
    assert(airplay_mfi_sap_respond(request, sizeof(request), response,
                                   sizeof(response), &response_len) ==
           AIRPLAY_MFI_SAP_SIGN_FAILED);

    airplay_mfi_sap_clear_provider();
    assert(airplay_mfi_sap_respond(request, sizeof(request), response,
                                   sizeof(response), &response_len) ==
           AIRPLAY_MFI_SAP_UNAVAILABLE);

    puts("airplay MFiSAP host test passed");
    return 0;
}
