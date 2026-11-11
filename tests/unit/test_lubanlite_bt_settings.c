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
static bool fail_append, locked;
static FILE *checked_open(const char *name, const char *mode)
{
    if (fail_append && !strcmp(name, store) && !strcmp(mode, "ab")) { errno = EIO; return NULL; }
    return fopen(name, mode);
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
static int load_key(const char *name, size_t len, settings_read_cb read_cb, void *arg)
{
    unsigned char value[256], direct[256];
    assert(!locked && name && len <= sizeof(value));
    assert(read_cb(arg, value, len) == (ssize_t)len);
    /* The host's h_set is allowed to call back into the settings core. */
    char path[64]; snprintf(path, sizeof(path), "bt/link_key/%s", name);
    assert(settings_load_one(path, direct, sizeof(direct)) == (ssize_t)len);
    assert(!memcmp(value, direct, len)); ++loaded;
    return 0;
}
static int commit_keys(void) { assert(!locked); ++committed; return 0; }
SETTINGS_STATIC_HANDLER_DEFINE(test_keys, "bt/link_key", NULL, load_key, commit_keys, NULL);
static void restart(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_ready = s_file_loaded = s_needs_compact = false;
    s_appends = 0; s_storage_error = 0;
    assert(!settings_subsys_init()); assert(!settings_load());
}
int main(void)
{
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
    unlink(store); unlink(temporary); unlink(backup); assert(!rmdir(directory));
    puts("Actual BT settings: reboot restore, reentrant handlers, compaction, torn suffix repair, I/O propagation, backup recovery and deletion passed");
}
