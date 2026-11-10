/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "carplay_mfi_provider.h"

#include <rtthread.h>

#include <stddef.h>

#include "airplay_mfi_sap.h"
#include "rtcp/rtcp.h"
#include "rtcp_lubanlite.h"

static struct rt_mutex s_lock;
static bool s_lock_ready;
static const struct rtcp_mfi *s_owner;
static unsigned s_users;

static int ensure_lock(void)
{
    if (s_lock_ready) {
        return RTCP_OK;
    }
    rt_enter_critical();
    if (!s_lock_ready) {
        if (rt_mutex_init(&s_lock, "mfi_sap", RT_IPC_FLAG_PRIO) != RT_EOK) {
            rt_exit_critical();
            return RTCP_EIO;
        }
        s_lock_ready = true;
    }
    rt_exit_critical();
    return RTCP_OK;
}

static int provider_random(uint8_t *output, size_t len, void *context)
{
    (void)context;
    return rtcp_lubanlite_random(output, len) ? RTCP_OK : RTCP_EIO;
}

/* 0x00000200 is the MFi 2.0C (SHA-1 digest, RSA-1024 signature);
 * 0x00000300 is an Auth 3.0 part (SHA-256).  When the probe has not
 * succeeded yet (lazy handle) keep the 2.0C default: it is the part on this
 * board, and the lazy handle re-probes at certificate time. */
static uint8_t protocol_major(const struct rtcp_mfi *mfi)
{
    struct rtcp_mfi_info info;
    if (mfi->info != NULL && mfi->info(&info) == RTCP_OK &&
        info.id32 == 0x00000300u) {
        return 3U;
    }
    return 2U;
}

int carplay_mfi_provider_install(const struct rtcp_mfi *mfi)
{
    if (mfi == NULL) {
        return RTCP_EINVAL;
    }
    int ret = ensure_lock();
    if (ret != RTCP_OK) {
        return ret;
    }
    if (rt_mutex_take(&s_lock, RT_WAITING_FOREVER) != RT_EOK) {
        return RTCP_EIO;
    }

    if (s_owner == mfi) {
        ++s_users;
        rt_mutex_release(&s_lock);
        return RTCP_OK;
    }
    if (s_owner != NULL) {
        /* Another coprocessor handle owns the provider.  Keep it: both
         * handles reach the same part on this board, and replacing it while
         * an AirPlay session can call /auth-setup would be worse. */
        rt_mutex_release(&s_lock);
        return RTCP_ESTATE;
    }

    uint8_t major = protocol_major(mfi);
    airplay_mfi_sap_provider_t provider = {
        .mfi = mfi,
        .protocol_major = major,
        .random = provider_random,
        .context = NULL,
    };
    if (!airplay_mfi_sap_set_provider(&provider)) {
        rt_mutex_release(&s_lock);
        return RTCP_ENOTSUP;
    }
    s_owner = mfi;
    s_users = 1U;
    rt_mutex_release(&s_lock);
    rt_kprintf("[mfi_sap] AirPlay /auth-setup provider installed "
               "(protocol major %u)\n", (unsigned)major);
    return RTCP_OK;
}

void carplay_mfi_provider_release(const struct rtcp_mfi *mfi)
{
    if (mfi == NULL || !s_lock_ready) {
        return;
    }
    if (rt_mutex_take(&s_lock, RT_WAITING_FOREVER) != RT_EOK) {
        return;
    }
    if (s_owner != mfi) {
        rt_mutex_release(&s_lock);
        return;
    }
    if (s_users > 1U) {
        --s_users;
        rt_mutex_release(&s_lock);
        return;
    }
    s_owner = NULL;
    s_users = 0U;
    airplay_mfi_sap_clear_provider();
    rt_mutex_release(&s_lock);
    rt_kprintf("[mfi_sap] AirPlay /auth-setup provider removed\n");
}
