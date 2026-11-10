/*
 * Apple MFi 2.0C register engine.
 *
 * This is the portable half of the Luban-Lite PoC: it contains no RT-Thread,
 * I2C-device, or shell code.  All bus access goes through rtcp_mfi2c_bus.
 *
 * Register sequence (verified on the board against a 2.0C whose ID32 reads
 * 0x00000200):
 *
 *   probe : vendor probe 31 bytes @ 0x4E, version 0x00..0x03,
 *           ID32 @ 0x04 (4 bytes, big-endian), error @ 0x05,
 *           certificate length @ 0x30 (2 bytes, big-endian)
 *   sign  : write {0x00, len} + challenge to 0x20, delay 10 ms,
 *           write 0x01 to 0x10, then wait for the result: bit 7 of 0x10 is
 *           the error flag, but the part under test leaves 0x10 at 0x00 even
 *           after the signature is ready (Apple's MFiPlatform_CreateSignature
 *           and the vendor ELF both read the status once and only check bit 7,
 *           then read 0x11/0x12 directly).  Completion is therefore detected
 *           by a plausible signature length @ 0x11 (2 bytes BE) followed by a
 *           successful signature read @ 0x12, with a ~2 s deadline.
 *   cert  : read length @ 0x30 (2 bytes BE), read DER @ 0x31
 *
 * The private key never leaves the coprocessor; this module has no API to
 * read or set identity material.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mfi2c.h"

#include <string.h>

#include "rtcp/log.h"

#define TAG "mfi2c"

#define MFI_REG_DEVICE_ID    0x04
#define MFI_REG_ERROR        0x05
#define MFI_REG_STATUS       0x10
#define MFI_REG_SIG_LEN      0x11
#define MFI_REG_SIG          0x12
#define MFI_REG_CHALLENGE    0x20
#define MFI_REG_CERT_LEN     0x30
#define MFI_REG_CERT         0x31
#define MFI_REG_VENDOR_PROBE 0x4E

#define MFI_VENDOR_PROBE_LEN 31
#define MFI_VENDOR_RETRIES   10
#define MFI_VENDOR_DELAY_MS  50

#define MFI_REG_RETRIES      3
#define MFI_REG_DELAY_MS     50

#define MFI_ID_2C            0x00000200u
#define MFI_ID_AUTH3         0x00000300u

#define MFI_STATUS_SUCCESS   0x10
#define MFI_STATUS_ERROR     0x80

/*
 * Poll pacing.  Every bus read uses the vendor 50 ms select-to-read gap
 * (MFI_VENDOR_DELAY_MS), so one poll costs roughly 110 ms.  18 polls give a
 * deadline just under Apple's 2 s signature timeout.
 */
#define MFI_STATUS_POLLS     18
#define MFI_STATUS_POLL_MS   10

/* A 2.0C is RSA-2048 (256-byte signature).  Anything shorter than this is
 * either the stale challenge window (0x00 0x14 = 20) or a not-yet-ready
 * register, never a real MFi signature. */
#define MFI_SIG_MIN_LEN      64

#define MFI_SIGN_DELAY_MS    10

static int mfi_read(rtcp_mfi2c_t *dev, uint8_t reg, uint8_t *buf, size_t len,
                    int retries, int delay_ms)
{
    if (dev == NULL || dev->bus == NULL || dev->bus->read == NULL)
        return RTCP_EINVAL;

    return dev->bus->read(dev->ctx, reg, buf, len, retries, delay_ms);
}

static int mfi_write(rtcp_mfi2c_t *dev, uint8_t reg, const uint8_t *data,
                     size_t len)
{
    if (dev == NULL || dev->bus == NULL || dev->bus->write == NULL)
        return RTCP_EINVAL;

    return dev->bus->write(dev->ctx, reg, data, len);
}

static void mfi_delay(rtcp_mfi2c_t *dev, unsigned ms)
{
    if (dev != NULL && dev->bus != NULL && dev->bus->delay_ms != NULL)
        dev->bus->delay_ms(dev->ctx, ms);
}

void rtcp_mfi2c_init(rtcp_mfi2c_t *dev, const struct rtcp_mfi2c_bus *bus,
                     void *ctx, uint8_t addr)
{
    if (dev == NULL)
        return;

    dev->bus = bus;
    dev->ctx = ctx;
    dev->addr = addr ? addr : RTCP_MFI2C_ADDR_DEFAULT;
}

int rtcp_mfi2c_probe(rtcp_mfi2c_t *dev, struct rtcp_mfi_info *info)
{
    uint8_t scratch[MFI_VENDOR_PROBE_LEN];
    uint8_t id4[4] = { 0 };
    uint8_t clen[2] = { 0 };
    uint8_t err = 0;
    int i;
    int have_id;
    int present = 0;

    if (info == NULL)
        return RTCP_EINVAL;

    memset(info, 0, sizeof(*info));

    if (mfi_read(dev, MFI_REG_VENDOR_PROBE, scratch, sizeof(scratch),
                 MFI_VENDOR_RETRIES, MFI_VENDOR_DELAY_MS) == RTCP_OK)
        present = 1;

    for (i = 0; i < 4; i++) {
        if (mfi_read(dev, (uint8_t)i, &info->version[i], 1,
                     MFI_REG_RETRIES, MFI_REG_DELAY_MS) == RTCP_OK)
            present = 1;
    }

    have_id = (mfi_read(dev, MFI_REG_DEVICE_ID, id4, sizeof(id4),
                        MFI_REG_RETRIES, MFI_REG_DELAY_MS) == RTCP_OK);
    if (have_id) {
        info->id32 = ((uint32_t)id4[0] << 24) | ((uint32_t)id4[1] << 16) |
                     ((uint32_t)id4[2] << 8) | (uint32_t)id4[3];
        present = 1;
    }

    if (mfi_read(dev, MFI_REG_ERROR, &err, 1,
                 MFI_REG_RETRIES, MFI_REG_DELAY_MS) == RTCP_OK)
        info->error = err;

    if (mfi_read(dev, MFI_REG_CERT_LEN, clen, sizeof(clen),
                 MFI_REG_RETRIES, MFI_REG_DELAY_MS) == RTCP_OK)
        info->cert_len = (uint16_t)(((uint16_t)clen[0] << 8) | clen[1]);

    info->present = present;

    return have_id ? RTCP_OK : RTCP_EIO;
}

int rtcp_mfi2c_sign(rtcp_mfi2c_t *dev, const uint8_t *challenge, size_t len,
                    uint8_t *sig, size_t sig_cap, size_t *sig_len)
{
    uint8_t buf[2 + RTCP_MFI_CHALLENGE_LEN];
    uint8_t start = 0x01;
    uint8_t status = 0;
    uint8_t slen[2] = { 0 };
    size_t n;
    int i;

    if (dev == NULL || challenge == NULL || sig == NULL || sig_len == NULL)
        return RTCP_EINVAL;
    if (len != RTCP_MFI_CHALLENGE_LEN) {
        RTCP_LOGE(TAG, "sign=challenge_length_unsupported len=%u expected=%u",
                  (unsigned)len, (unsigned)RTCP_MFI_CHALLENGE_LEN);
        return RTCP_EINVAL;
    }

    buf[0] = 0x00;
    buf[1] = (uint8_t)RTCP_MFI_CHALLENGE_LEN;
    memcpy(&buf[2], challenge, len);

    if (mfi_write(dev, MFI_REG_CHALLENGE, buf, sizeof(buf)) != RTCP_OK) {
        RTCP_LOGE(TAG, "sign=challenge_write_failed");
        return RTCP_EIO;
    }

    mfi_delay(dev, MFI_SIGN_DELAY_MS);

    if (mfi_write(dev, MFI_REG_STATUS, &start, 1) != RTCP_OK) {
        RTCP_LOGE(TAG, "sign=start_write_failed");
        return RTCP_EIO;
    }

    for (i = 0; i < MFI_STATUS_POLLS; i++) {
        mfi_delay(dev, MFI_STATUS_POLL_MS);

        /*
         * Use the vendor select-to-read gap here: while the coprocessor is
         * busy it drops register writes, and a dropped 0x10 select would make
         * the read return the previous register window.
         */
        status = 0;
        if (mfi_read(dev, MFI_REG_STATUS, &status, 1, 1,
                     MFI_VENDOR_DELAY_MS) == RTCP_OK && (status & MFI_STATUS_ERROR)) {
            RTCP_LOGE(TAG, "sign=status_error status=0x%02x poll=%d",
                      (unsigned)status, i);
            return RTCP_EIO;
        }

        n = 0;
        if (mfi_read(dev, MFI_REG_SIG_LEN, slen, sizeof(slen), 1,
                     MFI_VENDOR_DELAY_MS) == RTCP_OK) {
            n = ((size_t)slen[0] << 8) | slen[1];
            if (n >= MFI_SIG_MIN_LEN && n <= sig_cap) {
                if (mfi_read(dev, MFI_REG_SIG, sig, n, MFI_REG_RETRIES,
                             MFI_VENDOR_DELAY_MS) == RTCP_OK) {
                    *sig_len = n;
                    RTCP_LOGI(TAG, "sign: status=0x%02x siglen=%u polls=%d",
                              (unsigned)status, (unsigned)n, i + 1);
                    return RTCP_OK;
                }
            }
        }
    }

    RTCP_LOGE(TAG, "sign=status_timeout last_status=0x%02x last_siglen=%u polls=%u",
              (unsigned)status, (unsigned)n, (unsigned)MFI_STATUS_POLLS);
    return RTCP_ETIMEDOUT;
}

int rtcp_mfi2c_read_cert(rtcp_mfi2c_t *dev, uint8_t *der, size_t cap,
                         size_t *der_len)
{
    uint8_t clen[2] = { 0 };
    size_t n;

    if (dev == NULL || der == NULL || der_len == NULL)
        return RTCP_EINVAL;

    if (mfi_read(dev, MFI_REG_CERT_LEN, clen, sizeof(clen),
                 MFI_REG_RETRIES, MFI_REG_DELAY_MS) != RTCP_OK) {
        RTCP_LOGE(TAG, "cert=length_read_failed");
        return RTCP_EIO;
    }

    n = ((size_t)clen[0] << 8) | clen[1];
    if (n == 0 || n > cap || n > RTCP_MFI_CERT_MAX) {
        RTCP_LOGE(TAG, "cert=length_invalid len=%u cap=%u max=%u",
                  (unsigned)n, (unsigned)cap, (unsigned)RTCP_MFI_CERT_MAX);
        return RTCP_EIO;
    }

    if (mfi_read(dev, MFI_REG_CERT, der, n,
                 MFI_REG_RETRIES, MFI_REG_DELAY_MS) != RTCP_OK) {
        RTCP_LOGE(TAG, "cert=body_read_failed len=%u", (unsigned)n);
        return RTCP_EIO;
    }

    *der_len = n;
    return RTCP_OK;
}
