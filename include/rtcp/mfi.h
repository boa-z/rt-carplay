/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file mfi.h
 * Classic accessory authentication providers. */
#ifndef RTCP_MFI_H
#define RTCP_MFI_H

#include <stddef.h>
#include <stdint.h>

#define RTCP_MFI_CHALLENGE_LEN 20
#define RTCP_MFI_SIG_MAX       512
#define RTCP_MFI_CERT_MAX      1000
#define RTCP_MFI_WIRE_CERT_MAX 1536U

typedef enum
{
    RTCP_MFI_PROVIDER_HARDWARE = 1,
    RTCP_MFI_PROVIDER_SOFTWARE,
    RTCP_MFI_PROVIDER_EXTERNAL,
    RTCP_MFI_PROVIDER_TEST,
} rtcp_mfi_provider_kind_t;

enum
{
    RTCP_MFI_CAP_IAP2 = 1U << 0,
    RTCP_MFI_CAP_SAP = 1U << 1,
};

typedef struct
{
    rtcp_mfi_provider_kind_t kind;
    uint32_t capabilities;
    uint8_t protocol_major;
    size_t certificate_len; /**< Exact wire length, or zero if unavailable in metadata. */
} rtcp_mfi_provider_info_t;

/** Borrowed immutable provider and context. Keep its identity stable and
 * retain it until all iAP2 and Receiver owners have stopped using it.
 * The provider serializes shared mutable resources across owners and
 * bounds blocking operations. Callbacks return RTCP_OK or an RTCP error.
 * Input and output buffers are borrowed only for the duration of a call. */
typedef struct
{
    void *context;
    /** Describe the available identity. Return an error if it is unavailable. */
    int (*get_info)(void *context, rtcp_mfi_provider_info_t *out);
    /** Sign the classic protocol's prehashed challenge. Version 2 uses a
     * 20-byte SHA-1 digest and RSA PKCS#1 v1.5. Version 3 uses a 32-byte
     * SHA-256 digest and a 64-byte P-256 signature encoded as r || s. */
    int (*sign)(void *context, const uint8_t *challenge, size_t len,
                uint8_t *sig, size_t sig_cap, size_t *sig_len);
    /** Copy the wire certificate bytes and return the number written. */
    int (*read_cert)(void *context, uint8_t *der, size_t cap, size_t *der_len);
} rtcp_mfi_provider_t;

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
