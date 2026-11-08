/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * MFi 2.0C authentication coprocessor over the Luban-Lite I2C bus.
 *
 * The register engine lives in core/mfi/mfi2c.c; this file supplies the
 * rtcp_mfi2c_bus transaction layer and adapts it to struct rtcp_mfi for the
 * iAP2 session.
 *
 * Transaction shape (matching ZABlue's RJGT204 wrapper and the PoC):
 *   write: [register byte][data...] in ONE transfer, STOP.
 *   read : write the register byte (STOP), then read the data (STOP).
 * No repeated START is used; the coprocessor does not answer it.
 *
 * The demo128-nand-carplay board has the part on i2c0 at 0x6A (ZABlue's
 * HW_SECRUITY_I2C_ID=0 / HW_SECRUITY_I2C_ADDR=0x6A; i2c0 runs at 100 kHz).
 * The private key never leaves the coprocessor: this file can read the
 * certificate and ask for a signature, nothing else.
 */
#include "rtcp_lubanlite.h"

#include <string.h>

#if defined(__has_include)
#if __has_include(<rtconfig.h>)
#include <rtconfig.h>
#endif
#endif

#ifdef LPKG_RT_CARPLAY_LUBANLITE_MFI

#include <rtthread.h>

#include <drivers/i2c.h>

#include "aic_osal.h"
#include "mfi2c.h"
#include "rtcp/rtcp.h"

#define MFI_LUBANLITE_WRITE_MAX 160U

static struct {
    struct rt_i2c_bus_device *bus;
    uint8_t address;
    rtcp_mfi2c_t device;
    struct rtcp_mfi_info info;
    bool ready;
} s_mfi;

static int bus_write(void *context, uint8_t reg, const uint8_t *data,
                     size_t len)
{
    struct rt_i2c_bus_device *bus = context;
    if (bus == NULL || data == NULL || len == 0U ||
        len > MFI_LUBANLITE_WRITE_MAX) {
        return RTCP_EINVAL;
    }

    uint8_t frame[MFI_LUBANLITE_WRITE_MAX + 1U];
    frame[0] = reg;
    memcpy(&frame[1], data, len);
    rt_size_t sent = rt_i2c_master_send(bus, s_mfi.address, RT_I2C_WR, frame,
                                        (rt_uint32_t)(len + 1U));
    return sent == len + 1U ? RTCP_OK : RTCP_EIO;
}

static int bus_read(void *context, uint8_t reg, uint8_t *data, size_t len,
                    int retries, int delay_ms)
{
    struct rt_i2c_bus_device *bus = context;
    if (bus == NULL || data == NULL || len == 0U) return RTCP_EINVAL;

    for (int attempt = 0; attempt <= retries; ++attempt) {
        if (attempt > 0 && delay_ms > 0) {
            aicos_msleep((uint32_t)delay_ms);
        }
        rt_size_t got = rt_i2c_read_reg(bus, s_mfi.address, reg, data,
                                        (rt_uint32_t)len);
        if (got == len) return RTCP_OK;
    }
    return RTCP_EIO;
}

static void bus_delay(void *context, unsigned ms)
{
    (void)context;
    aicos_msleep(ms);
}

static const struct rtcp_mfi2c_bus s_bus_ops = {
    .write = bus_write,
    .read = bus_read,
    .delay_ms = bus_delay,
};

static const struct rtcp_mfi s_mfi_ops;

int rtcp_mfi_lubanlite_start(const char *i2c_bus_name, uint8_t address)
{
    if (s_mfi.ready) return RTCP_OK;
    if (i2c_bus_name == NULL || i2c_bus_name[0] == '\0') return RTCP_EINVAL;

    s_mfi.bus = rt_i2c_bus_device_find(i2c_bus_name);
    if (s_mfi.bus == NULL) {
        RTCP_LOGE("mfi", "I2C bus %s not found", i2c_bus_name);
        return RTCP_ENOENT;
    }
    s_mfi.address = address != 0U ? address : RTCP_MFI2C_ADDR_DEFAULT;
    rtcp_mfi2c_init(&s_mfi.device, &s_bus_ops, s_mfi.bus, s_mfi.address);

    int result = rtcp_mfi2c_probe(&s_mfi.device, &s_mfi.info);
    if (result != RTCP_OK) {
        RTCP_LOGE("mfi", "no coprocessor at %s 0x%02x (%d)", i2c_bus_name,
                  (unsigned)s_mfi.address, result);
        return result;
    }

    s_mfi.ready = true;
    RTCP_LOGI("mfi", "2.0C at %s 0x%02x: id=0x%08x version=%02x.%02x.%02x.%02x "
              "cert=%u%s", i2c_bus_name, (unsigned)s_mfi.address,
              (unsigned)s_mfi.info.id32,
              s_mfi.info.version[0], s_mfi.info.version[1],
              s_mfi.info.version[2], s_mfi.info.version[3],
              (unsigned)s_mfi.info.cert_len,
              s_mfi.info.error != 0U ? " (error flag set)" : "");
    return RTCP_OK;
}

bool rtcp_mfi_lubanlite_ready(void)
{
    return s_mfi.ready;
}

const struct rtcp_mfi *rtcp_mfi_lubanlite(void)
{
    return s_mfi.ready ? &s_mfi_ops : NULL;
}

/* ------------------------------------------------------------------ */
/* struct rtcp_mfi adapter                                             */
/* ------------------------------------------------------------------ */

static int mfi_info(struct rtcp_mfi_info *out)
{
    if (out == NULL) return RTCP_EINVAL;
    if (!s_mfi.ready) return RTCP_ESTATE;
    *out = s_mfi.info;
    return RTCP_OK;
}

static int mfi_sign(const uint8_t *challenge, size_t len, uint8_t *sig,
                    size_t sig_cap, size_t *sig_len)
{
    if (!s_mfi.ready) return RTCP_ESTATE;
    return rtcp_mfi2c_sign(&s_mfi.device, challenge, len, sig, sig_cap,
                           sig_len);
}

static int mfi_read_cert(uint8_t *der, size_t cap, size_t *der_len)
{
    if (!s_mfi.ready) return RTCP_ESTATE;
    return rtcp_mfi2c_read_cert(&s_mfi.device, der, cap, der_len);
}

static const struct rtcp_mfi s_mfi_ops = {
    .info = mfi_info,
    .sign = mfi_sign,
    .read_cert = mfi_read_cert,
};

#endif /* LPKG_RT_CARPLAY_LUBANLITE_MFI */
