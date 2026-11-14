/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_MFI2C_H
#define RTCP_MFI2C_H

#include <stddef.h>
#include <stdint.h>

#include "rtcp/rtcp.h"
#include "rtcp/mfi.h"

#define RTCP_MFI2C_ADDR_DEFAULT 0x10

/*
 * The bus callback implements the vendor transaction shape:
 *
 *   write(reg, data, len): ONE transfer, reg byte followed by data, STOP.
 *   read (reg, data, len): write the register byte (STOP), wait, then read
 *                          (STOP), retried `retries` times with `delay_ms`.
 *
 * Getting that shape right matters: the 2.0C does not answer a repeated-start
 * combined write/read on this board (verified in the PoC).
 */
struct rtcp_mfi2c_bus {
    int  (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    int  (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len,
                 int retries, int delay_ms);
    void (*delay_ms)(void *ctx, unsigned ms);
};

typedef struct rtcp_mfi2c {
    const struct rtcp_mfi2c_bus *bus;
    void  *ctx;
    uint8_t addr;
} rtcp_mfi2c_t;

void rtcp_mfi2c_init(rtcp_mfi2c_t *dev, const struct rtcp_mfi2c_bus *bus,
                     void *ctx, uint8_t addr);

int rtcp_mfi2c_probe(rtcp_mfi2c_t *dev, struct rtcp_mfi_info *info);
int rtcp_mfi2c_sign(rtcp_mfi2c_t *dev, const uint8_t *challenge, size_t len,
                    uint8_t *sig, size_t sig_cap, size_t *sig_len);
int rtcp_mfi2c_read_cert(rtcp_mfi2c_t *dev, uint8_t *der, size_t cap,
                         size_t *der_len);

#endif /* RTCP_MFI2C_H */
