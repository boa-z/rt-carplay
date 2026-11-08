/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ArtInChip CE true random number generator as an rtcp_os.random provider.
 *
 * The CE HAL exposes the TRNG as one more crypto task (ALG_TRNG = 0x50); no
 * in-tree driver used it before this port.  The completion bit is awaited on
 * accelerator unit 3 because hal_crypto_init() clears four ISR bits and the
 * three known engines (symmetric, hash, asymmetric) own bits 0..2.  That is
 * inferred from hal_ce.c, not board-verified: if the inference is wrong the
 * poll times out and Pair-Setup refuses to run instead of accepting weak
 * entropy, which is the safe failure.  Verify on hardware before relying on
 * wireless CarPlay pairing.
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
#include <hal_ce.h>

#define TRNG_UNIT 3U
#define TRNG_CHUNK_BYTES 32U
#define TRNG_TIMEOUT_US 200000U

bool rtcp_lubanlite_random(uint8_t *output, size_t len)
{
    if (output == NULL || len == 0U) return false;
    if (hal_crypto_init() != 0) return false;

    /* The CE writes through the cache, so the output buffer is cache-line
     * sized and aligned; only the requested prefix is copied out. */
    uint8_t block[CACHE_LINE_SIZE] __attribute__((aligned(CACHE_LINE_SIZE)));
    while (len != 0U) {
        size_t chunk = len < TRNG_CHUNK_BYTES ? len : TRNG_CHUNK_BYTES;
        struct crypto_task task __attribute__((aligned(CACHE_LINE_SIZE)));
        memset(&task, 0, sizeof(task));
        task.alg.trng.alg_tag = ALG_TRNG;
        task.data.out_addr = (u32)(uintptr_t)block;
        task.data.out_len = TRNG_CHUNK_BYTES;

        aicos_dcache_clean_range((void *)&task, sizeof(task));
        aicos_dcache_invalid_range(block, sizeof(block));
        hal_crypto_start_symm(&task);
        if (hal_crypto_poll_finish(TRNG_UNIT, TRNG_TIMEOUT_US) != 0) {
            memset(block, 0, sizeof(block));
            return false;
        }
        hal_crypto_pending_clear(TRNG_UNIT);
        if (hal_crypto_get_err(TRNG_UNIT) != 0U) {
            hal_crypto_err_clear(TRNG_UNIT);
            memset(block, 0, sizeof(block));
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
    (void)output;
    (void)len;
    return false;
}

#endif
