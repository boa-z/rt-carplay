/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static char store[256], temporary[256], backup[256];
static bool fail_append, fail_read, negative_errno, locked;
static FILE *checked_open(const char *name, const char *mode)
{
    if ((fail_append && !strcmp(name, store) && !strcmp(mode, "ab")) ||
        (fail_read && !strcmp(mode, "rb"))) {
        errno = negative_errno ? -EIO : EIO;
        return NULL;
    }
    FILE *file = fopen(name, mode);
    if (!file && negative_errno && errno > 0) errno = -errno;
    return file;
}
#define SETTINGS_FILE store
#define SETTINGS_FILE_TMP temporary
#define SETTINGS_FILE_BAK backup
#define fopen checked_open
#include "../../port/luban-lite/rtcp_bt_settings.c"
#undef fopen
rt_mutex_t rt_mutex_create(const char *name, int flags) { (void)name; (void)flags; return &locked; }
int rt_mutex_take(rt_mutex_t m, int wait) { (void)m; (void)wait; assert(!locked); locked = true; return 0; }
int rt_mutex_release(rt_mutex_t m) { (void)m; assert(locked); locked = false; return 0; }
static unsigned loaded, committed;
static int commit_error;
static bool reject_restore;
static int load_key(const char *name, size_t len, settings_read_cb read_cb, void *arg)
{
    unsigned char value[256], direct[256];
    assert(!locked && name && len <= sizeof(value));
    if (reject_restore) return -ENOENT;
    assert(read_cb(arg, value, len) == (ssize_t)len);
    /* The host's h_set is allowed to call back into the settings core. */
    char path[64]; snprintf(path, sizeof(path), "bt/link_key/%s", name);
    assert(settings_load_one(path, direct, sizeof(direct)) == (ssize_t)len);
    assert(!memcmp(value, direct, len)); ++loaded;
    return 0;
}
static int commit_keys(void) { assert(!locked); ++committed; return commit_error; }
SETTINGS_STATIC_HANDLER_DEFINE(test_keys, "bt/link_key", NULL, load_key, commit_keys, NULL);
static void restart(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_ready = s_file_loaded = s_needs_compact = false;
    s_appends = 0; s_storage_error = 0;
    assert(!settings_subsys_init()); assert(!settings_load());
}
int main(int argc, char **argv)
{
    (void)argv;
    negative_errno = argc > 1;
    char directory[] = "/tmp/rt-carplay-bonds-XXXXXX";
    assert(mkdtemp(directory));
    snprintf(store, sizeof(store), "%s/bonds", directory);
    snprintf(temporary, sizeof(temporary), "%s/bonds.tmp", directory);
    snprintf(backup, sizeof(backup), "%s/bonds.bak", directory);
    unsigned char key[16] = {1,2,3}, read[16];
    restart(); assert(!rtcp_bt_settings_error());
    assert(!settings_save_one("bt/link_key/phone", key, sizeof(key)));
    restart(); assert(loaded == 1 && committed == 2);
    assert(settings_load_one("bt/link_key/phone", read, sizeof(read)) == 16 && !memcmp(key, read, 16));
    for (unsigned i = 0; i < 100; ++i) { key[0] = (unsigned char)i; assert(!settings_save_one("bt/link_key/phone", key, 16)); }
    restart(); assert(settings_load_one("bt/link_key/phone", read, 16) == 16 && read[0] == 99);
    FILE *file = fopen(store, "ab"); assert(file);
    assert(fwrite("bad", 1, 3, file) == 3 && !fclose(file));
    restart(); key[0] = 100;
    assert(!settings_save_one("bt/link_key/phone", key, 16));
    restart(); assert(settings_load_one("bt/link_key/phone", read, 16) == 16 && read[0] == 100);
    fail_append = true;
    assert(settings_save_one("bt/link_key/phone2", key, 16) == -EIO);
    assert(rtcp_bt_settings_error() == -EIO);
    fail_append = false;
    assert(!settings_save_one("bt/link_key/phone2", key, 16)); /* repair snapshot */
    assert(!rtcp_bt_settings_error());
    assert(!rename(store, backup)); restart(); /* interrupted FAT promotion */
    assert(settings_load_one("bt/link_key/phone2", read, 16) == 16);
    assert(!settings_delete("bt/link_key/phone")); restart();
    assert(settings_load_one("bt/link_key/phone", read, 16) == -ENOENT);
    assert(settings_load_one("bt/link_key/phone2", read, 16) == 16);
    assert(!settings_save_one("bt/id", key, 16));
    assert(!rtcp_bt_settings_forget_link_keys());
    restart();
    assert(settings_load_one("bt/link_key/phone2", read, 16) == -ENOENT);
    assert(settings_load_one("bt/id", read, 16) == 16);
    /* A rejected BR record must not be reported as restored. */
    assert(!settings_save_one("bt/link_key/phone", key, 16));
    reject_restore = true;
    assert(settings_load() == -ENOENT);
    assert(rtcp_bt_settings_error() == -ENOENT);
    assert(settings_load_one("bt/link_key/phone", read, 16) == 16);
    reject_restore = false;
    commit_error = -EIO;
    assert(settings_load() == -EIO && rtcp_bt_settings_error() == -EIO);
    commit_error = 0;
    restart();
    /* An actual read error still prevents init and preserves the file.
     * Retrying after recovery must restore the existing local identity. */
    s_ready = s_file_loaded = false;
    memset(s_entries, 0, sizeof(s_entries));
    fail_read = true;
    assert(settings_subsys_init() == -EIO && !s_ready);
    fail_read = false;
    assert(!settings_subsys_init() && !settings_load());
    assert(settings_load_one("bt/id", read, 16) == 16);
    /* A store made by a larger configuration must not be silently
     * truncated/compacted when this firmware has fewer table entries. */
    file = fopen(store, "wb"); assert(file);
    for (unsigned i = 0; i < SETTINGS_MAX_ENTRIES + 1; ++i) {
        char name[64]; uint8_t header[6];
        snprintf(name, sizeof(name), "bt/link_key/peer%u", i);
        size_t n = record_encode(header, name, 16);
        assert(fwrite(header, 1, 6, file) == 6);
        assert(fwrite(name, 1, n, file) == n);
        assert(fwrite(key, 1, 16, file) == 16);
    }
    long original_size = ftell(file);
    assert(!fclose(file));
    s_ready = s_file_loaded = false;
    memset(s_entries, 0, sizeof(s_entries));
    assert(settings_subsys_init() == -EIO && !s_ready);
    file = fopen(store, "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END) && ftell(file) == original_size);
    assert(!fclose(file));
    unlink(store); unlink(temporary); unlink(backup); assert(!rmdir(directory));
    puts("Actual BT settings: reboot restore, reentrant handlers, compaction, torn suffix repair, I/O propagation, backup recovery and deletion passed");
    printf("First-boot missing files and actual I/O failure/retry passed with %s errno\n",
           negative_errno ? "Luban-Lite negative" : "POSIX positive");
}
