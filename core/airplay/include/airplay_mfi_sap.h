/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtcp/mfi.h"

#define AIRPLAY_MFI_SAP_MAX_CERTIFICATE_BYTES 1536U
#define AIRPLAY_MFI_SAP_MAX_SIGNATURE_BYTES 256U
#define AIRPLAY_MFI_SAP_MAX_RESPONSE_BYTES \
    (32U + 4U + AIRPLAY_MFI_SAP_MAX_CERTIFICATE_BYTES + 4U + \
     AIRPLAY_MFI_SAP_MAX_SIGNATURE_BYTES)

typedef enum {
    AIRPLAY_MFI_SAP_OK = 0,
    AIRPLAY_MFI_SAP_INVALID_ARGUMENT,
    AIRPLAY_MFI_SAP_UNAVAILABLE,
    AIRPLAY_MFI_SAP_UNSUPPORTED_PROTOCOL,
    AIRPLAY_MFI_SAP_CRYPTO_FAILED,
    AIRPLAY_MFI_SAP_SIGN_FAILED,
    AIRPLAY_MFI_SAP_BUFFER_TOO_SMALL,
} airplay_mfi_sap_result_t;

/*
 * Certificate and challenge-signing operations are injected.  protocol_major
 * is the MFi version reported by the coprocessor: 2 = MFi 2.0C (SHA-1
 * digest), 3 = Auth 3.0 (SHA-256 digest).  random may be NULL only for host
 * tests that provide deterministic bytes through the coprocessor itself.
 */
typedef struct {
    const struct rtcp_mfi *mfi;
    uint8_t protocol_major;
    int (*random)(uint8_t *output, size_t len, void *context);
    void *context;
} airplay_mfi_sap_provider_t;

/* Registers the process-wide MFi provider.  Returns false on invalid input. */
bool airplay_mfi_sap_set_provider(const airplay_mfi_sap_provider_t *provider);
void airplay_mfi_sap_clear_provider(void);

/* Builds the MFiSAP /auth-setup response body from an iPhone request body. */
airplay_mfi_sap_result_t airplay_mfi_sap_respond(
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len);
