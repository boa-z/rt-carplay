/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file mfi_software.h
 * Software provider for an application-provisioned classic MFi identity. */
#ifndef RTCP_MFI_SOFTWARE_H
#define RTCP_MFI_SOFTWARE_H

#include "rtcp/mfi.h"
#include "rtcp/os.h"

#define RTCP_MFI_SOFTWARE_KEY_MAX 4096U

typedef struct rtcp_mfi_software rtcp_mfi_software_t;

typedef struct
{
    /** Exact wire certificate bytes, DER certificates-only PKCS#7 or DER X.509. */
    const uint8_t *certificate;
    size_t certificate_len;
    /** Unencrypted DER key or NUL-terminated PEM including the terminator. */
    const uint8_t *private_key;
    size_t private_key_len;
    /** 2 (RSA/SHA-1), 3 (P-256/SHA-256), or 0 to derive from the key. */
    uint8_t protocol_major;
    /** Optional secure RNG override. Return 0 on success. Its context stays
     * borrowed until destruction. If absent, use os->random. */
    int (*random)(void *context, uint8_t *output, size_t len);
    void *random_context;
    /** Platform allocation and mutex operations, borrowed until destruction. */
    const struct rtcp_os *os;
} rtcp_mfi_software_config_t;

/** Copy and check an authorized identity before returning a provider. Input
 * buffers may be cleared after return. The host provisions the authorized
 * chain. The peer independently validates acceptance and trust. Returns
 * RTCP_ENOTSUP when this backend or the input algorithm is unsupported.
 * On failure *out is set to NULL. */
int rtcp_mfi_software_create(const rtcp_mfi_software_config_t *config,
                             rtcp_mfi_software_t **out);

/** Borrow the provider until destruction. Returns NULL for a NULL instance. */
const rtcp_mfi_provider_t *rtcp_mfi_software_provider(rtcp_mfi_software_t *software);

/** Release parsed private key storage after all owners have retired. */
void rtcp_mfi_software_destroy(rtcp_mfi_software_t *software);

#endif
