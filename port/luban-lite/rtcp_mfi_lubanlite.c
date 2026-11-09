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
 * The demo128-nand-carplay board has the Apple 2.0C on i2c0 at 0x10
 * (0x11 also answers; i2c0 runs at 100 kHz).  The 0x6A device on the same
 * bus is the vendor RJGT204 device lock, not the MFi coprocessor.
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
        /* The register write and the data read are two separate transfers,
         * each with STOP, and the part needs a settling gap between them.
         * The vendor driver does write -> 50 ms -> read; rt_i2c_read_reg()
         * issues the two transfers back to back, which the board's 2.0C NAKs.
         * Keep the vendor timing here. */
        rt_size_t sent = rt_i2c_master_send(bus, s_mfi.address, RT_I2C_WR,
                                            &reg, 1U);
        if (sent == 1U) {
            if (delay_ms > 0) aicos_msleep((uint32_t)delay_ms);
            rt_size_t got = rt_i2c_master_recv(bus, s_mfi.address, 0U, data,
                                               (rt_uint32_t)len);
            if (got == len) return RTCP_OK;
        } else if (delay_ms > 0) {
            aicos_msleep((uint32_t)delay_ms);
        }
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

    /* A part that ACKs can still answer nonsense; the first diagnostic build
     * printed id=0x32323232 version=32.32.32.32 cert=12850.  Only the values
     * that identify an Apple coprocessor make it usable. */
    if (s_mfi.info.id32 != 0x00000200u && s_mfi.info.id32 != 0x00000300u &&
        s_mfi.info.cert_len != 908u) {
        RTCP_LOGE("mfi", "%s 0x%02x answered but not like a 2.0C: "
                  "id=0x%08x version=%02x.%02x.%02x.%02x cert=%u; run "
                  "'carplay_mfi' for a raw bus dump", i2c_bus_name,
                  (unsigned)s_mfi.address, (unsigned)s_mfi.info.id32,
                  s_mfi.info.version[0], s_mfi.info.version[1],
                  s_mfi.info.version[2], s_mfi.info.version[3],
                  (unsigned)s_mfi.info.cert_len);
        return RTCP_EIO;
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
/* Bring-up diagnostic: raw 2.0C bus dump                              */
/* ------------------------------------------------------------------ */

/*
 * The probe reports what the register engine managed to read.  When those
 * values do not look like an Apple part, this dump shows the raw bus
 * behaviour with all three transaction shapes tried on this board:
 *
 *   vendor    : [reg] STOP, 50 ms, read STOP  (the driver's shape)
 *   immediate : [reg] STOP, read STOP, no gap
 *   combined  : one transfer with WR+RD messages (repeated-start attempt)
 *
 * and scans 0x08..0x77 the way the earlier PoC mfi_scan did, so the result
 * can be compared with the board-verified 2026-11-08 output.  It never
 * starts authentication and never reads the certificate body.
 */
static int diag_read_shape(struct rt_i2c_bus_device *bus, uint8_t addr,
                           uint8_t reg, uint8_t *buf, size_t len, int delay_ms)
{
    rt_size_t sent = rt_i2c_master_send(bus, addr, RT_I2C_WR, &reg, 1U);

    if (sent != 1U) return -1;
    if (delay_ms > 0) aicos_msleep((uint32_t)delay_ms);
    return rt_i2c_master_recv(bus, addr, 0U, buf, (rt_uint32_t)len) == len
               ? 0 : -1;
}

static int diag_read_combined(struct rt_i2c_bus_device *bus, uint8_t addr,
                              uint8_t reg, uint8_t *buf, size_t len)
{
    struct rt_i2c_msg msgs[2];

    msgs[0].addr = addr;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].len = 1;
    msgs[0].buf = &reg;
    msgs[1].addr = addr;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].len = (rt_uint16_t)len;
    msgs[1].buf = buf;
    return rt_i2c_transfer(bus, msgs, 2U) == 2U ? 0 : -1;
}

static void diag_dump(const char *what, int ok, const uint8_t *buf, size_t len)
{
    int i;

    rt_kprintf("[mfi] %s %s:", what, ok ? "FAIL" : "ok  ");
    for (i = 0; i < (int)len; i++)
        rt_kprintf(" %02x", buf[i]);
    rt_kprintf("\n");
}

void rtcp_mfi_lubanlite_dump(const char *i2c_bus_name, uint8_t address)
{
    struct rt_i2c_bus_device *bus;
    uint8_t buf[31];
    int pass, ok, a, acked = 0;

    if (i2c_bus_name == NULL || i2c_bus_name[0] == '\0')
        i2c_bus_name = "i2c0";
    if (address == 0U) address = RTCP_MFI2C_ADDR_DEFAULT;

    bus = rt_i2c_bus_device_find(i2c_bus_name);
    if (bus == NULL) {
        rt_kprintf("[mfi] bus %s not found\n", i2c_bus_name);
        return;
    }

    rt_kprintf("[mfi] dump on %s addr 0x%02x\n", i2c_bus_name, address);

    memset(buf, 0xAA, sizeof(buf));
    ok = diag_read_shape(bus, address, 0x4E, buf, sizeof(buf), 50);
    diag_dump("reg 0x4E len 31 vendor   ", ok, buf, 8);

    for (pass = 0; pass < 2; pass++) {
        memset(buf, 0xAA, sizeof(buf));
        ok = diag_read_shape(bus, address, 0x04, buf, 4, 50);
        diag_dump(pass == 0 ? "reg 0x04 len 4  vendor   "
                            : "reg 0x04 len 4  vendor#2 ", ok, buf, 4);
    }

    memset(buf, 0xAA, sizeof(buf));
    ok = diag_read_shape(bus, address, 0x04, buf, 4, 0);
    diag_dump("reg 0x04 len 4  immediate", ok, buf, 4);

    memset(buf, 0xAA, sizeof(buf));
    ok = diag_read_combined(bus, address, 0x04, buf, 4);
    diag_dump("reg 0x04 len 4  combined ", ok, buf, 4);

    memset(buf, 0xAA, sizeof(buf));
    ok = diag_read_shape(bus, address, 0x00, buf, 1, 50);
    diag_dump("reg 0x00 len 1  vendor   ", ok, buf, 1);

    memset(buf, 0xAA, sizeof(buf));
    ok = diag_read_shape(bus, address, 0x05, buf, 1, 50);
    diag_dump("reg 0x05 len 1  vendor   ", ok, buf, 1);

    memset(buf, 0xAA, sizeof(buf));
    ok = diag_read_shape(bus, address, 0x30, buf, 2, 50);
    diag_dump("reg 0x30 len 2  vendor   ", ok, buf, 2);

    rt_kprintf("[mfi] scan %s:", i2c_bus_name);
    for (a = 0x08; a <= 0x77; a++) {
        uint8_t v = 0;

        if (diag_read_shape(bus, (uint8_t)a, 0x00, &v, 1, 5) == 0) {
            rt_kprintf(" 0x%02x=0x%02x", a, v);
            acked++;
        }
    }
    rt_kprintf(" (%d ACK)\n", acked);
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
