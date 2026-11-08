/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_MFI_H
#define RTCP_MFI_H

#include <stddef.h>
#include <stdint.h>

#define RTCP_MFI_CHALLENGE_LEN 20
#define RTCP_MFI_SIG_MAX       512
#define RTCP_MFI_CERT_MAX      1000

struct rtcp_mfi_info {
    uint32_t id32;          /* 0x00000200 = 2.0C, 0x00000300 = Auth 3.0 */
    uint8_t  version[4];
    uint16_t cert_len;
    uint8_t  error;
    int      present;
};

struct rtcp_mfi {
    int (*info)(struct rtcp_mfi_info *out);
    int (*sign)(const uint8_t *challenge, size_t len,
                uint8_t *sig, size_t sig_cap, size_t *sig_len);
    int (*read_cert)(uint8_t *der, size_t cap, size_t *der_len);
};

#endif /* RTCP_MFI_H */
