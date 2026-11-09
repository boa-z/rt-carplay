/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ArtInChip CE true random number generator as an rtcp_os.random provider.
 *
 * The CE HAL exposes the TRNG as one more crypto task (ALG_TRNG = 0x50).
 * The completion is awaited on the hash accelerator bit, matching the SDK's
 * own TRNG implementation:
 *   packages/artinchip/aic-authorization/authorization/rng.c
 * which starts the task with hal_crypto_start_hash() and polls
 * ALG_HASH_ACCELERATOR.  The first board run showed that waiting on unit 3
 * times out (the initial unit-3 guess came from hal_crypto_init() clearing
 * four ISR bits).  The task also carries in_len = 4 ("default is 4 double
 * words" in the SDK reference).
 *
 * On any failure the whole output is zeroed and false is returned, so a
 * caller that ignores the result never consumes stale stack bytes.
 */
#include "rtcp_lubanlite.h"

#include <stdint.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<rtconfig.h>)
#include <rtconfig.h>
#endif
#endif

#include "rtcp/rtcp.h"

#if defined(AIC_CE_DRV) && defined(DRIVER_HAL_EN)

#include <aic_core.h>
#include <aic_hal.h>
#include <aic_log.h>
#include <hal_ce.h>

#define TRNG_UNIT ALG_HASH_ACCELERATOR
#define TRNG_CHUNK_BYTES 32U
#define TRNG_TIMEOUT_US 300000U

bool rtcp_lubanlite_random(uint8_t *output, size_t len)
{
    if (output == NULL || len == 0U) return false;
    uint8_t *const baseline = output;
    const size_t total = len;

    if (hal_crypto_init() != 0) {
        pr_err("rtcp: failed to enable the CE clock\n");
        memset(baseline, 0, total);
        return false;
    }

    /* The CE writes through the cache, so the output buffer is cache-line
     * sized and aligned; only the requested prefix is copied out. */
    uint8_t block[CACHE_LINE_SIZE] __attribute__((aligned(CACHE_LINE_SIZE)));
    while (len != 0U) {
        size_t chunk = len < TRNG_CHUNK_BYTES ? len : TRNG_CHUNK_BYTES;
        struct crypto_task task __attribute__((aligned(CACHE_LINE_SIZE)));
        memset(&task, 0, sizeof(task));
        task.alg.trng.alg_tag = ALG_TRNG;
        task.data.in_len = 4U; /* SDK TRNG reference: 4 double words */
        task.data.out_addr = (u32)(uintptr_t)block;
        task.data.out_len = TRNG_CHUNK_BYTES;

        aicos_dcache_clean_range((void *)&task, sizeof(task));
        aicos_dcache_invalid_range(block, sizeof(block));
        hal_crypto_start_hash(&task);
        if (hal_crypto_poll_finish(TRNG_UNIT, TRNG_TIMEOUT_US) != 0) {
            pr_err("rtcp: CE TRNG timed out\n");
            memset(block, 0, sizeof(block));
            memset(baseline, 0, total);
            return false;
        }
        hal_crypto_pending_clear(TRNG_UNIT);
        if (hal_crypto_get_err(TRNG_UNIT) != 0U) {
            pr_err("rtcp: CE TRNG reported an error\n");
            hal_crypto_err_clear(TRNG_UNIT);
            memset(block, 0, sizeof(block));
            memset(baseline, 0, total);
            return false;
        }
        aicos_dma_sync();
        memcpy(output, block, chunk);
        output += chunk;
        len -= chunk;
    }
    memset(block, 0, sizeof(block));
    return true;
}

#else

bool rtcp_lubanlite_random(uint8_t *output, size_t len)
{
    if (output != NULL) memset(output, 0, len);
    return false;
}

#endif
