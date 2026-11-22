/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file mfi.h
 * Authentication coprocessor provider. */
#ifndef RTCP_MFI_H
#define RTCP_MFI_H

#include <stddef.h>
#include <stdint.h>

#define RTCP_MFI_CHALLENGE_LEN 20
#define RTCP_MFI_SIG_MAX       512
#define RTCP_MFI_CERT_MAX      1000

/** Snapshot of detected coprocessor identity, certificate size and status. */
struct rtcp_mfi_info
{
    uint32_t id32; /* 0x00000200 = 2.0C, 0x00000300 = Auth 3.0 */
    uint8_t version[4];
    uint16_t cert_len;
    uint8_t error;
    int present;
};

/** Borrowed iAP2 signing provider. Operations run synchronously on the
 * link owner and return RTCP_OK or a negative error. A shared physical
 * chip must serialize operations in its platform adapter. */
struct rtcp_mfi
{
    int (*info)(struct rtcp_mfi_info *out);
    /** Fill sig within sig_cap and return the actual length via sig_len. */
    int (*sign)(const uint8_t *challenge, size_t len,
                uint8_t *sig, size_t sig_cap, size_t *sig_len);
    /** Copy DER bytes to caller storage and report their length. */
    int (*read_cert)(uint8_t *der, size_t cap, size_t *der_len);
};

#endif /* RTCP_MFI_H */
