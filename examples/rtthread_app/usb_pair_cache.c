/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Local, versioned ABI-tagged record, including this host's private keys.
 * Size tag rejects incompatible builds. Checksum detects accidental corruption,
 * not tampering. Files stay on /data and keys are never printed. */
#include "usb_pair_cache.h"
#include "pairing_files.h"
#include "mbedtls/platform_util.h"
#include <stdlib.h>
#include <string.h>
#define HEADER 16U
#define SIZE (HEADER + sizeof(rtcp_pair_record_t))
static uint32_t checksum(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619U;
    return h;
}
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void write32(uint8_t *p, uint32_t v) { for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i)); }
int carplay_usb_pair_cache_load(const char *path, rtcp_pair_record_t *record) {
    uint8_t *blob = malloc(SIZE);
    if (!blob) return RTCP_ENOMEM;
    size_t n = 0;
    int ret = carplay_file_load(path, blob, SIZE, &n);
    if (ret == RTCP_OK) {
        if (n == 0) ret = RTCP_ENOENT;
        else if (n != SIZE || memcmp(blob, "RCPUSB01", 8) ||
                 read32(blob+8) != sizeof(*record) || read32(blob+12) != checksum(blob+HEADER,sizeof(*record))) ret = RTCP_EIO;
        else {
            memcpy(record, blob+HEADER, sizeof(*record));
            if (!rtcp_pair_record_valid(record)) ret = RTCP_EIO;
        }
    }
    if (ret != RTCP_OK) memset(record, 0, sizeof(*record));
    mbedtls_platform_zeroize(blob, SIZE); free(blob); return ret;
}
int carplay_usb_pair_cache_save(const char *path, const rtcp_pair_record_t *record) {
    if (!rtcp_pair_record_valid(record)) return RTCP_EINVAL;
    uint8_t *blob = malloc(SIZE);
    if (!blob) return RTCP_ENOMEM;
    memcpy(blob,"RCPUSB01",8); write32(blob+8,sizeof(*record));
    memcpy(blob+HEADER,record,sizeof(*record));
    write32(blob+12,checksum(blob+HEADER,sizeof(*record)));
    int ret = carplay_file_save(path,blob,SIZE);
    mbedtls_platform_zeroize(blob,SIZE); free(blob); return ret;
}
