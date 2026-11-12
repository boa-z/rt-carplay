/*
 * Minimal Zephyr settings storage for the Luban-Lite Bluetooth port.
 *
 * The zephyr-bluetooth package in this SDK ships the settings API header and
 * the Bluetooth host's settings handlers, but no settings core.  With
 * CONFIG_LPKG_BT_SETTINGS the host persists the BR/EDR link keys through
 * settings_save_one()/settings_delete().  Without a core those calls are
 * unresolved, and without persistence every reboot invalidates the bond the
 * iPhone stored: iOS then finds the device but cannot reconnect ("Connection
 * Unsuccessful") until the user forgets and pairs it again.
 *
 * This file implements the subset the Bluetooth host uses, backed by a small
 * append-only file on the RT-Thread DFS.  Values are also kept in RAM, so the
 * stack keeps working (without persistence) when /data is not mounted.
 *
 * File record layout (little endian):
 *
 *   u16 magic (0x5E77) | u16 value_len | u8 name_len | u8 reserved
 *   name[name_len] | value[value_len]
 *
 * value_len == 0xFFFF is a deletion tombstone.  A torn or unknown record ends
 * the replay; the file is compacted after too many appends.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <rtthread.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/iterable_sections.h>

#include "rtcp/log.h"

#define TAG "bt-settings"

#ifndef SETTINGS_FILE
#define SETTINGS_FILE        "/data/bt_settings.bin"
#define SETTINGS_FILE_TMP    "/data/bt_settings.bin.tmp"
#define SETTINGS_FILE_BAK    "/data/bt_settings.bin.bak"
#endif
#define SETTINGS_MAGIC       0x5E77U
#define SETTINGS_TOMBSTONE   0xFFFFU
#define SETTINGS_MAX_ENTRIES 16
#define SETTINGS_NAME_MAX    64
/* CONFIG_BT_SETTINGS_CCC_STORE_MAX (48) entries of 4-byte structs need 192
 * bytes for the "bt/ccc/<addr>" value; BR keys also carry flags/padding. */
#define SETTINGS_VALUE_MAX   256
#define SETTINGS_APPEND_MAX  (SETTINGS_MAX_ENTRIES * 4)

struct settings_entry {
    bool used;
    char name[SETTINGS_NAME_MAX];
    size_t len;
    uint8_t value[SETTINGS_VALUE_MAX];
};

static struct settings_entry s_entries[SETTINGS_MAX_ENTRIES];
static rt_mutex_t s_lock;
static bool s_ready;
static bool s_file_loaded;
static unsigned s_appends;
static int s_storage_error;
static bool s_needs_compact;

int rtcp_bt_settings_error(void) { return s_storage_error; }

/* ------------------------------------------------------------------ */
/* Name helpers                                                        */
/* ------------------------------------------------------------------ */

int settings_name_next(const char *name, const char **next)
{
    size_t i = 0U;

    if (next != NULL) {
        *next = NULL;
    }
    if (name == NULL) {
        return 0;
    }
    while (name[i] != '\0' && name[i] != '/') {
        i++;
    }
    if (next != NULL && name[i] == '/') {
        *next = &name[i + 1U];
    }
    return (int)i;
}

int settings_name_steq(const char *name, const char *key, const char **next)
{
    size_t i = 0U;

    if (next != NULL) {
        *next = NULL;
    }
    if (name == NULL || key == NULL) {
        return 0;
    }
    while (key[i] != '\0') {
        if (name[i] != key[i]) {
            return 0;
        }
        i++;
    }
    if (name[i] == '\0') {
        if (next != NULL) {
            *next = &name[i];
        }
        return 1;
    }
    if (name[i] == '/') {
        if (next != NULL) {
            *next = &name[i + 1U];
        }
        return 1;
    }
    return 0;
}

struct settings_handler_static *settings_parse_and_lookup(const char *name,
                                                          const char **next)
{
    struct settings_handler_static *best = NULL;
    const char *best_next = NULL;
    size_t best_len = 0U;

    if (name == NULL) {
        return NULL;
    }

    /* The BT host registers "bt", "bt/link_key" and "bt/keys"; the longest
     * matching subtree owns the key. */
    STRUCT_SECTION_FOREACH(settings_handler_static, handler) {
        const char *candidate = NULL;
        size_t len;

        if (handler->name == NULL) {
            continue;
        }
        if (!settings_name_steq(name, handler->name, &candidate)) {
            continue;
        }
        len = strlen(handler->name);
        if (best == NULL || len > best_len) {
            best = handler;
            best_next = candidate;
            best_len = len;
        }
    }

    if (best != NULL && next != NULL) {
        *next = best_next;
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* RAM table and file backend                                          */
/* ------------------------------------------------------------------ */

static struct settings_entry *entry_find(const char *name)
{
    for (size_t i = 0U; i < SETTINGS_MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].name, name) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static struct settings_entry *entry_alloc(const char *name)
{
    for (size_t i = 0U; i < SETTINGS_MAX_ENTRIES; i++) {
        if (!s_entries[i].used) {
            s_entries[i].used = true;
            strncpy(s_entries[i].name, name, SETTINGS_NAME_MAX - 1U);
            s_entries[i].name[SETTINGS_NAME_MAX - 1U] = '\0';
            s_entries[i].len = 0U;
            return &s_entries[i];
        }
    }
    return NULL;
}

static bool entry_set(const char *name, const void *value, size_t len)
{
    struct settings_entry *entry;

    if (strlen(name) >= SETTINGS_NAME_MAX || len > SETTINGS_VALUE_MAX) {
        RTCP_LOGW(TAG, "key too large: %s (%u bytes)", name,
                  (unsigned)len);
        return false;
    }

    entry = entry_find(name);
    if (entry == NULL) {
        entry = entry_alloc(name);
    }
    if (entry == NULL) {
        RTCP_LOGW(TAG, "table full, cannot store %s", name);
        return false;
    }
    memcpy(entry->value, value, len);
    entry->len = len;
    return true;
}

static void entry_delete(const char *name)
{
    struct settings_entry *entry = entry_find(name);
    if (entry != NULL) {
        entry->used = false;
    }
}

static size_t record_encode(uint8_t *header, const char *name, uint16_t len)
{
    uint16_t magic = SETTINGS_MAGIC;

    header[0] = (uint8_t)(magic & 0xFFU);
    header[1] = (uint8_t)(magic >> 8);
    header[2] = (uint8_t)(len & 0xFFU);
    header[3] = (uint8_t)(len >> 8);
    header[4] = (uint8_t)strlen(name);
    header[5] = 0U;
    return (size_t)header[4];
}

static int file_append(const char *name, const void *value, uint16_t len)
{
    FILE *file;
    uint8_t header[6];
    size_t name_len;
    bool ok;

    file = fopen(SETTINGS_FILE, "ab");
    if (file == NULL) {
        RTCP_LOGW(TAG, "cannot open %s (persistence disabled)", SETTINGS_FILE);
        return -EIO;
    }

    name_len = record_encode(header, name, len);
    ok = fwrite(header, 1U, sizeof(header), file) == sizeof(header) &&
         fwrite(name, 1U, name_len, file) == name_len;
    if (ok && len != SETTINGS_TOMBSTONE && len != 0U) {
        ok = fwrite(value, 1U, len, file) == len;
    }
    if (fclose(file) != 0) {
        ok = false;
    }

    if (ok) {
        s_appends++;
    }
    return ok ? 0 : -EIO;
}

static int file_compact(void)
{
    FILE *file = fopen(SETTINGS_FILE_TMP, "wb");
    bool ok = true;

    if (file == NULL) {
        return -EIO;
    }

    for (size_t i = 0U; i < SETTINGS_MAX_ENTRIES && ok; i++) {
        uint8_t header[6];
        size_t name_len;
        uint16_t len;

        if (!s_entries[i].used) {
            continue;
        }
        len = (uint16_t)s_entries[i].len;
        name_len = record_encode(header, s_entries[i].name, len);
        ok = fwrite(header, 1U, sizeof(header), file) == sizeof(header) &&
             fwrite(s_entries[i].name, 1U, name_len, file) == name_len &&
             fwrite(s_entries[i].value, 1U, len, file) == len;
    }

    if (fclose(file) != 0 || !ok) {
        remove(SETTINGS_FILE_TMP);
        return -EIO;
    }
    if (rename(SETTINGS_FILE_TMP, SETTINGS_FILE) != 0) {
        /* FAT may not replace an existing destination. Preserve the old
         * store until promotion succeeds, including across power loss. */
        remove(SETTINGS_FILE_BAK);
        bool old = rename(SETTINGS_FILE, SETTINGS_FILE_BAK) == 0;
        if (rename(SETTINGS_FILE_TMP, SETTINGS_FILE) != 0) {
            if (old) (void)rename(SETTINGS_FILE_BAK, SETTINGS_FILE);
            remove(SETTINGS_FILE_TMP);
            return -EIO;
        }
    }
    remove(SETTINGS_FILE_BAK);
    s_appends = 0U; s_needs_compact = false;
    return 0;
}

static int file_compact_if_needed(void)
{
    return s_needs_compact || s_appends > SETTINGS_APPEND_MAX ? file_compact() : 0;
}

static bool file_load(void)
{
    FILE *file = fopen(SETTINGS_FILE, "rb");

    if (file == NULL) {
        /* Luban-Lite DFS/newlib passes through negative errno, whereas
         * POSIX hosts use positive errno. First boot is valid in either
         * case; other open failures must not discard existing bonds. */
        int error = errno;
        if (error != ENOENT && error != -ENOENT) {
            RTCP_LOGE(TAG, "open failed path=%s errno=%d", SETTINGS_FILE, error);
            return false;
        }
        file = fopen(SETTINGS_FILE_BAK, "rb");
        if (file == NULL) {
            error = errno;
            if (error == ENOENT || error == -ENOENT) return true;
            RTCP_LOGE(TAG, "open failed path=%s errno=%d", SETTINGS_FILE_BAK, error);
            return false;
        }
        s_needs_compact = true;
    }

    for (;;) {
        uint8_t header[6];
        uint16_t magic;
        uint16_t len;
        uint8_t name_len;
        char name[SETTINGS_NAME_MAX];
        uint8_t value[SETTINGS_VALUE_MAX];

        size_t got = fread(header, 1U, sizeof(header), file);
        if (got != sizeof(header)) {
            if (got || ferror(file)) s_needs_compact = true;
            break;
        }
        magic = (uint16_t)(header[0] | ((uint16_t)header[1] << 8));
        len = (uint16_t)(header[2] | ((uint16_t)header[3] << 8));
        name_len = header[4];

        if (magic != SETTINGS_MAGIC || name_len == 0U ||
            name_len >= SETTINGS_NAME_MAX ||
            (len != SETTINGS_TOMBSTONE && len > SETTINGS_VALUE_MAX)) {
            s_needs_compact = true;
            break;
        }

        if (fread(name, 1U, name_len, file) != name_len) {
            s_needs_compact = true;
            break;
        }
        name[name_len] = '\0';

        if (len != SETTINGS_TOMBSTONE && len != 0U) {
            if (fread(value, 1U, len, file) != len) {
                s_needs_compact = true;
                break;
            }
        }

        if (len == SETTINGS_TOMBSTONE) {
            entry_delete(name);
        } else {
            if (!entry_set(name, value, len)) {
                /* Never compact an incomplete RAM snapshot over the store:
                 * doing so permanently loses bonds beyond the table limit. */
                RTCP_LOGE(TAG, "settings table full while replaying %s", name);
                fclose(file);
                return false;
            }
        }
    }
    fclose(file);
    s_storage_error = file_compact_if_needed();
    return true;
}

/* ------------------------------------------------------------------ */
/* Zephyr lib helpers used by the BT host settings code                 */
/*                                                                      */
/* This SDK builds only a subset of the zephyr-bluetooth package, so    */
/* lib/utils.c is not linked; the settings host code needs these three. */
/* ------------------------------------------------------------------ */

int hex2char(uint8_t x, char *c)
{
    if (x >= 0x10U) {
        return -EINVAL;
    }
    *c = (char)(x < 0x0AU ? (unsigned)'0' + x : (unsigned)'a' + (x - 0x0AU));
    return 0;
}

int char2hex(char c, uint8_t *x)
{
    if (c >= '0' && c <= '9') {
        *x = (uint8_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
        *x = (uint8_t)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
        *x = (uint8_t)(c - 'A' + 10);
    } else {
        return -EINVAL;
    }
    return 0;
}

size_t hex2bin(const char *hex, size_t hexlen, uint8_t *buf, size_t buflen)
{
    size_t n = 0U;

    for (size_t i = 0U; i < hexlen && n < buflen; i += 2U) {
        uint8_t high;
        uint8_t low;

        if (char2hex(hex[i], &high) != 0) {
            break;
        }
        if (i + 1U >= hexlen) {
            buf[n] = (uint8_t)(high << 4);
        } else {
            if (char2hex(hex[i + 1U], &low) != 0) {
                break;
            }
            buf[n] = (uint8_t)((high << 4) | low);
        }
        n++;
    }
    return n;
}

uint8_t u8_to_dec(char *buf, uint8_t buflen, uint8_t value)
{
    uint8_t digits = 1U;
    uint8_t temp = value;

    while (temp >= 10U) {
        temp /= 10U;
        digits++;
    }
    if (buflen <= digits) {
        return 0U;
    }
    for (uint8_t i = digits; i > 0U; i--) {
        buf[i - 1U] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    buf[digits] = '\0';
    return digits;
}

/* ------------------------------------------------------------------ */
/* Settings API                                                        */
/* ------------------------------------------------------------------ */

int settings_subsys_init(void)
{
    unsigned count = 0U;

    if (s_ready) {
        return 0;
    }

    if (s_lock == NULL) {
        s_lock = rt_mutex_create("bt-settings", RT_IPC_FLAG_FIFO);
        if (s_lock == NULL) {
            return -ENOMEM;
        }
    }

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    /* An absent file means first boot. An unreadable store must remain an
     * error instead of silently replacing the saved phone identities. */
    if (!s_file_loaded) {
        s_file_loaded = file_load();
        if (!s_file_loaded) s_storage_error = -EIO;
    }
    for (size_t i = 0U; i < SETTINGS_MAX_ENTRIES; i++) {
        if (s_entries[i].used) {
            count++;
        }
    }
    rt_mutex_release(s_lock);

    if (!s_file_loaded) return -EIO;
    s_ready = true;
    RTCP_LOGI(TAG, "settings storage ready (%s, %u entries)",
              SETTINGS_FILE, count);
    return 0;
}

ssize_t settings_load_one(const char *name, void *buf, size_t buf_len)
{
    struct settings_entry *entry;
    ssize_t result = -ENOENT;
    size_t n;

    if (!s_ready || name == NULL) {
        return -EINVAL;
    }
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    entry = entry_find(name);
    if (entry != NULL) {
        n = entry->len < buf_len ? entry->len : buf_len;
        if (buf != NULL) {
            memcpy(buf, entry->value, n);
        }
        result = (ssize_t)entry->len;
    }
    rt_mutex_release(s_lock);
    return result;
}

ssize_t settings_get_val_len(const char *key)
{
    return settings_load_one(key, NULL, 0U);
}

static ssize_t read_cb_from_mem(void *cb_arg, void *data, size_t len)
{
    const struct settings_entry *entry = cb_arg;
    size_t n;

    if (entry == NULL) {
        return -EINVAL;
    }
    n = entry->len < len ? entry->len : len;
    if (data != NULL) {
        memcpy(data, entry->value, n);
    }
    return (ssize_t)n;
}

int settings_load_subtree_direct(const char *subtree,
                                 settings_load_direct_cb cb, void *param)
{
    if (!s_ready || subtree == NULL || cb == NULL) {
        return -EINVAL;
    }

    for (size_t i = 0U; i < SETTINGS_MAX_ENTRIES; i++) {
        struct settings_entry snapshot;
        const char *next = NULL;
        int rc;

        rt_mutex_take(s_lock, RT_WAITING_FOREVER);
        if (!s_entries[i].used ||
            !settings_name_steq(s_entries[i].name, subtree, &next)) {
            rt_mutex_release(s_lock);
            continue;
        }
        snapshot = s_entries[i];
        rt_mutex_release(s_lock);

        rc = cb(snapshot.name, snapshot.len, read_cb_from_mem, &snapshot,
                param);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

int settings_load_subtree(const char *subtree)
{
    int result = 0;
    if (!s_ready || subtree == NULL) {
        return -EINVAL;
    }

    for (size_t i = 0U; i < SETTINGS_MAX_ENTRIES; i++) {
        struct settings_entry snapshot;
        struct settings_handler_static *handler;
        const char *next = NULL;

        rt_mutex_take(s_lock, RT_WAITING_FOREVER);
        if (!s_entries[i].used ||
            !settings_name_steq(s_entries[i].name, subtree, &next)) {
            rt_mutex_release(s_lock);
            continue;
        }
        snapshot = s_entries[i];
        handler = settings_parse_and_lookup(snapshot.name, &next);
        rt_mutex_release(s_lock);

        if (handler != NULL && handler->h_set != NULL) {
            int rc = handler->h_set(next, snapshot.len, read_cb_from_mem, &snapshot);
            if (rc && !result) result = rc;
        }
    }
    return result;
}

static int settings_commit_all(void)
{
    int rc = 0;

    STRUCT_SECTION_FOREACH(settings_handler_static, handler) {
        if (handler->h_commit != NULL) {
            int err = handler->h_commit();
            if (err != 0 && rc == 0) {
                rc = err;
            }
        }
    }
    return rc;
}

int settings_load(void)
{
    int result = 0;
    if (!s_ready) {
        return -EINVAL;
    }

    /* Dispatch one entry at a time without holding s_lock: a handler can
     * store or delete settings (link_key_set clears invalid keys, and the
     * bt commit re-saves identities), which would deadlock otherwise. */
    for (size_t i = 0U; i < SETTINGS_MAX_ENTRIES; i++) {
        struct settings_entry snapshot;
        struct settings_handler_static *handler;
        const char *next = NULL;

        rt_mutex_take(s_lock, RT_WAITING_FOREVER);
        if (!s_entries[i].used) {
            rt_mutex_release(s_lock);
            continue;
        }
        snapshot = s_entries[i];
        handler = settings_parse_and_lookup(snapshot.name, &next);
        rt_mutex_release(s_lock);

        if (handler != NULL && handler->h_set != NULL) {
            int rc = handler->h_set(next, snapshot.len, read_cb_from_mem, &snapshot);
            if (rc && !result) result = rc;
            if (rc) RTCP_LOGE(TAG, "restore failed key=%s bytes=%u rc=%d",
                             snapshot.name, (unsigned)snapshot.len, rc);
        } else if (!strncmp(snapshot.name, "bt/link_key/", strlen("bt/link_key/"))) {
            /* A linker/configuration error must not look like a restored
             * bond. iOS still holds the matching key on its side. */
            RTCP_LOGE(TAG, "BR bond handler missing key=%s", snapshot.name);
            if (!result) result = -ENOENT;
        }
    }

    int committed = settings_commit_all();
    if (result || committed) s_storage_error = result ? result : committed;
    return result ? result : committed;
}

int settings_save_one(const char *name, const void *value, size_t val_len)
{
    bool stored;
    int rc;

    if (name == NULL || value == NULL || val_len == 0U) {
        return -EINVAL;
    }
    if (val_len > SETTINGS_VALUE_MAX || strlen(name) >= SETTINGS_NAME_MAX) {
        return -EINVAL;
    }
    if (!s_ready) {
        return -EINVAL;
    }

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    stored = entry_set(name, value, val_len);
    rc = stored ? 0 : -ENOMEM;
    if (stored) {
        rc = s_needs_compact ? file_compact() : file_append(name, value, (uint16_t)val_len);
        if (!rc) rc = file_compact_if_needed();
        if (rc) s_needs_compact = true;
    }
    s_storage_error = rc;
    rt_mutex_release(s_lock);

    return rc;
}

int settings_delete(const char *name)
{
    if (name == NULL || !s_ready) {
        return -EINVAL;
    }

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    entry_delete(name);
    int rc = s_needs_compact ? file_compact() : file_append(name, NULL, SETTINGS_TOMBSTONE);
    if (!rc) rc = file_compact_if_needed();
    if (rc) s_needs_compact = true;
    s_storage_error = rc;
    rt_mutex_release(s_lock);
    return rc;
}

/* Preserve bt/id and any LE settings. The caller first removes live BR bonds
 * through the host API; rewrite once so ignored host delete errors cannot
 * resurrect a bond on the next boot. Also works before the radio is enabled. */
int rtcp_bt_settings_forget_link_keys(void)
{
    int rc = settings_subsys_init();
    if (rc) return rc;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    for (size_t i = 0; i < SETTINGS_MAX_ENTRIES; ++i) {
        if (s_entries[i].used &&
            !strncmp(s_entries[i].name, "bt/link_key/", strlen("bt/link_key/")))
            memset(&s_entries[i], 0, sizeof(s_entries[i]));
    }
    rc = file_compact();
    s_needs_compact = rc != 0;
    s_storage_error = rc;
    rt_mutex_release(s_lock);
    return rc;
}

int settings_commit(void)
{
    if (!s_ready) {
        return -EINVAL;
    }
    return settings_commit_all();
}

int settings_commit_subtree(const char *subtree)
{
    (void)subtree;
    return settings_commit();
}

int settings_save(void)
{
    if (!s_ready) {
        return -EINVAL;
    }
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    int rc = file_compact();
    s_needs_compact = rc != 0;
    s_storage_error = rc;
    rt_mutex_release(s_lock);
    return rc;
}

int settings_save_subtree(const char *subtree)
{
    (void)subtree;
    return settings_save();
}

int settings_register(struct settings_handler *cf)
{
    (void)cf;
    return -ENOTSUP;
}

int settings_register_with_cprio(struct settings_handler *cf, int cprio)
{
    (void)cf;
    (void)cprio;
    return -ENOTSUP;
}

int settings_runtime_set(const char *name, const void *data, size_t len)
{
    return settings_save_one(name, data, len);
}

int settings_runtime_get(const char *name, void *data, size_t len)
{
    ssize_t got = settings_load_one(name, data, len);
    return got < 0 ? (int)got : 0;
}

int settings_runtime_commit(const char *name)
{
    (void)name;
    return settings_commit();
}

int settings_storage_get(void **storage)
{
    if (storage != NULL) {
        *storage = NULL;
    }
    return -ENOTSUP;
}
