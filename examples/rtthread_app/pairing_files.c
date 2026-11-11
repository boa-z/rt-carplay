/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FatFS f_rename cannot overwrite a destination. Keep the previous committed
 * copy as .bak, recover it if power is lost between the two renames. This is
 * best-effort crash recovery, not a filesystem transaction/fsync guarantee. */
#include "pairing_files.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include "rtcp/rtcp.h"
#include "rtcp/log.h"
#define PATH_BYTES 256U
static int suffix(char out[PATH_BYTES], const char *path, const char *ending)
{
    int n = snprintf(out, PATH_BYTES, "%s%s", path, ending);
    return n > 0 && n < (int)PATH_BYTES ? RTCP_OK : RTCP_EINVAL;
}
int carplay_file_load(const char *path, uint8_t *data, size_t capacity, size_t *length)
{
    if (!path || !data || !length) return RTCP_EINVAL;
    *length = 0;
    FILE *f = fopen(path, "rb");
    if (!f && errno == ENOENT) {
        char backup[PATH_BYTES];
        if (suffix(backup, path, ".bak") != RTCP_OK) return RTCP_EINVAL;
        f = fopen(backup, "rb");
    }
    if (!f) return errno == ENOENT ? RTCP_OK : RTCP_EIO;
    size_t got = fread(data, 1, capacity, f);
    bool ok = !ferror(f) && fgetc(f) == EOF;
    if (ferror(f)) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) return RTCP_EIO;
    *length = got;
    return RTCP_OK;
}
int carplay_file_save(const char *path, const uint8_t *data, size_t length)
{
    char temporary[PATH_BYTES], backup[PATH_BYTES];
    if (!path || !data || suffix(temporary, path, ".tmp") != RTCP_OK ||
        suffix(backup, path, ".bak") != RTCP_OK) return RTCP_EINVAL;
    FILE *f = fopen(temporary, "wb");
    if (!f) goto fail;
    bool ok = fwrite(data, 1, length, f) == length;
    if (fclose(f) != 0) ok = false;
    if (!ok) goto fail;
    f = fopen(path, "rb");
    bool had_primary = f != NULL;
    if (f) fclose(f);
    else if (errno != ENOENT) goto fail;
    if (had_primary) {
        if (remove(backup) != 0 && errno != ENOENT) goto fail;
        if (rename(path, backup) != 0) goto fail;
    }
    if (rename(temporary, path) == 0) return RTCP_OK;
    if (had_primary) (void)rename(backup, path);
fail:
    RTCP_LOGE("pairing_file", "save failed path=%s errno=%d", path, errno);
    (void)remove(temporary);
    return RTCP_EIO;
}
