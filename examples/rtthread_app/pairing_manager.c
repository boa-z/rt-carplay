/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pairing_manager.h"
#include "rtcp/rtcp.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
bool carplay_pair_filename_allowed(const char *name)
{
    if (!name) return false;
    if (!strcmp(name, "rt-carplay-pairings.bin")) return true;
    const char *prefix = "rt-carplay-usb-";
    size_t n = strlen(name), p = strlen(prefix);
    if (n < p + 16 + 4 || n > p + 64 + 4 || strncmp(name, prefix, p) ||
        strcmp(name + n - 4, ".bin")) return false;
    for (size_t i = p; i < n - 4; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '-')) return false;
    }
    return true;
}
int carplay_pair_files_scan(const char *directory, carplay_pair_file_t *files,
                            unsigned capacity, unsigned *count, bool *more)
{
    if (!directory || !files || !count || !more) return RTCP_EINVAL;
    *count = 0; *more = false;
    DIR *dir = opendir(directory);
    if (!dir) return RTCP_EIO;
    int ret = RTCP_OK;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) ret = RTCP_EIO; break; }
        char name[CARPLAY_PAIR_FILENAME_MAX];
        size_t n = strlen(entry->d_name);
        if (n >= sizeof(name)) continue;
        memcpy(name, entry->d_name, n + 1);
        if (n > 4 && (!strcmp(name + n - 4, ".bak") || !strcmp(name + n - 4, ".tmp")))
            name[n - 4] = 0;
        if (!carplay_pair_filename_allowed(name)) continue;
        unsigned i;
        for (i = 0; i < *count; ++i) if (!strcmp(files[i].name, name)) break;
        if (i != *count) continue;
        if (*count >= capacity) { *more = true; continue; }
        char path[256];
        int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
        if (length < 0 || length >= (int)sizeof(path)) { ret = RTCP_EINVAL; break; }
        struct stat st;
        if (stat(path, &st) != 0) {
            snprintf(path, sizeof(path), "%s/%s.bak", directory, name);
            if (stat(path, &st) != 0) {
                snprintf(path, sizeof(path), "%s/%s.tmp", directory, name);
                if (stat(path, &st) != 0) continue;
            }
        }
        if (!S_ISREG(st.st_mode)) continue;
        snprintf(files[*count].name, sizeof(files[*count].name), "%s", name);
        files[*count].bytes = (size_t)st.st_size;
        ++*count;
    }
    if (closedir(dir) != 0) ret = RTCP_EIO;
    return ret;
}
int carplay_pair_file_forget(const char *directory, const char *name)
{
    if (!directory || !carplay_pair_filename_allowed(name)) return RTCP_EINVAL;
    /* Remove recovery copies first, so a partially failed deletion never
     * resurrects a backup after a successful removal of the primary. */
    static const char *suffixes[] = {".tmp", ".bak", ""};
    for (unsigned i = 0; i < 3; ++i) {
        char path[256];
        int n = snprintf(path, sizeof(path), "%s/%s%s", directory, name, suffixes[i]);
        if (n < 0 || n >= (int)sizeof(path)) return RTCP_EINVAL;
        if (remove(path) != 0 && errno != ENOENT) return RTCP_EIO;
    }
    return RTCP_OK;
}
