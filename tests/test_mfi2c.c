/*
 * Host-side test for the MFi 2.0C register engine using a fake bus.
 * No hardware, RTOS, or I2C code is involved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mfi2c.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake {
    uint8_t status;
    uint8_t challenge[22];
    uint8_t sig[128];
    uint8_t cert[908];
    int     fail_sign;
};

static int fake_write(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    struct fake *f = ctx;

    if (reg == 0x20 && len == 22) {
        memcpy(f->challenge, data, 22);
        return RTCP_OK;
    }

    if (reg == 0x10 && len == 1 && data[0] == 0x01) {
        if (f->fail_sign) {
            f->status = 0x80;
        } else {
            f->status = 0x10;
            f->sig[0] = 0xA5;
            f->sig[127] = 0x5A;
        }
        return RTCP_OK;
    }

    return RTCP_EIO;
}

static int fake_read(void *ctx, uint8_t reg, uint8_t *data, size_t len,
                     int retries, int delay_ms)
{
    struct fake *f = ctx;
    size_t i;

    (void)retries;
    (void)delay_ms;

    switch (reg) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        if (len != 1)
            return RTCP_EIO;
        data[0] = 0x05;
        return RTCP_OK;
    case 0x04:
        if (len != 4)
            return RTCP_EIO;
        data[0] = 0x00; data[1] = 0x00; data[2] = 0x02; data[3] = 0x00;
        return RTCP_OK;
    case 0x05:
        if (len != 1)
            return RTCP_EIO;
        data[0] = 0x00;
        return RTCP_OK;
    case 0x10:
        if (len != 1)
            return RTCP_EIO;
        data[0] = f->status;
        return RTCP_OK;
    case 0x11:
        if (len != 2)
            return RTCP_EIO;
        data[0] = 0x00; data[1] = 0x80;
        return RTCP_OK;
    case 0x12:
        if (len != sizeof(f->sig))
            return RTCP_EIO;
        memcpy(data, f->sig, sizeof(f->sig));
        return RTCP_OK;
    case 0x30:
        if (len != 2)
            return RTCP_EIO;
        data[0] = 0x03; data[1] = 0x8C;      /* 908 */
        return RTCP_OK;
    case 0x31:
        if (len != sizeof(f->cert))
            return RTCP_EIO;
        memcpy(data, f->cert, sizeof(f->cert));
        return RTCP_OK;
    case 0x4E:
        for (i = 0; i < len; i++)
            data[i] = 0x5A;
        return RTCP_OK;
    default:
        return RTCP_EIO;
    }
}

static void fake_delay(void *ctx, unsigned ms)
{
    (void)ctx;
    (void)ms;
}

static const struct rtcp_mfi2c_bus fake_bus = {
    .write = fake_write,
    .read = fake_read,
    .delay_ms = fake_delay,
};

int main(void)
{
    struct fake f;
    struct rtcp_mfi_info info;
    rtcp_mfi2c_t dev;
    uint8_t challenge[RTCP_MFI_CHALLENGE_LEN];
    uint8_t sig[RTCP_MFI_SIG_MAX];
    uint8_t cert[RTCP_MFI_CERT_MAX];
    size_t sig_len = 0;
    size_t cert_len = 0;
    size_t i;

    memset(&f, 0, sizeof(f));
    for (i = 0; i < sizeof(f.cert); i++)
        f.cert[i] = (uint8_t)i;

    rtcp_mfi2c_init(&dev, &fake_bus, &f, 0x10);

    assert(rtcp_mfi2c_probe(&dev, &info) == RTCP_OK);
    assert(info.present == 1);
    assert(info.id32 == 0x00000200);
    assert(info.cert_len == 908);
    assert(info.version[0] == 0x05);

    for (i = 0; i < sizeof(challenge); i++)
        challenge[i] = (uint8_t)(0x10 + i);

    assert(rtcp_mfi2c_sign(&dev, challenge, sizeof(challenge),
                           sig, sizeof(sig), &sig_len) == RTCP_OK);
    assert(sig_len == 128);
    assert(sig[0] == 0xA5);
    assert(sig[127] == 0x5A);
    assert(f.challenge[0] == 0x00);
    assert(f.challenge[1] == 0x14);
    assert(memcmp(&f.challenge[2], challenge, sizeof(challenge)) == 0);

    assert(rtcp_mfi2c_read_cert(&dev, cert, sizeof(cert), &cert_len) == RTCP_OK);
    assert(cert_len == 908);
    assert(cert[0] == 0x00 && cert[907] == (uint8_t)907);

    /* Wrong challenge length must be rejected before touching the bus. */
    assert(rtcp_mfi2c_sign(&dev, challenge, RTCP_MFI_CHALLENGE_LEN - 1,
                           sig, sizeof(sig), &sig_len) == RTCP_EINVAL);

    /* Error bit 7 in the status register must abort the signature. */
    f.fail_sign = 1;
    assert(rtcp_mfi2c_sign(&dev, challenge, sizeof(challenge),
                           sig, sizeof(sig), &sig_len) == RTCP_EIO);

    puts("test_mfi2c: OK");
    return 0;
}
