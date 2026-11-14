/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../../examples/rtthread_headless/headless.h"
#include "airplay_bplist.h"
#include "airplay_ed25519.h"
#include "airplay_pairing_store.h"
#include "mdns_browser.h"
#include "rtcp_linux.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Real filesystem calls with modeled DFS errno signs and FatFS no-overwrite.
 * Only the selected temporary->primary commit is fault-injected. */
static unsigned filesystem_mode;
static bool fail_commit, stop_on_ready;
extern FILE *__real_fopen(const char *, const char *);
extern int __real_remove(const char *);
extern int __real_rename(const char *, const char *);
static void filesystem_errno(void)
{
    if (filesystem_mode && errno > 0) errno = -errno;
}
FILE *__wrap_fopen(const char *path, const char *mode)
{
    FILE *file = __real_fopen(path, mode);
    if (!file) filesystem_errno();
    return file;
}
int __wrap_remove(const char *path)
{
    int rc = __real_remove(path);
    if (rc != 0) filesystem_errno();
    return rc;
}
int __wrap_rename(const char *from, const char *to)
{
    size_t length = strlen(from);
    if (fail_commit && length > 4 && !strcmp(from + length - 4, ".tmp")) {
        fail_commit = false; errno = EIO; filesystem_errno(); return -1;
    }
    if (filesystem_mode == 2 && access(to, F_OK) == 0) {
        errno = EEXIST; filesystem_errno(); return -1;
    }
    int rc = __real_rename(from, to);
    if (rc != 0) filesystem_errno();
    return rc;
}

static atomic_bool stop_requested = ATOMIC_VAR_INIT(false);
static atomic_uint ready_port = ATOMIC_VAR_INIT(0);
static atomic_uint state_events = ATOMIC_VAR_INIT(0);
static rtcp_headless_config_t config;
static rtcp_headless_result_t result;
static uint8_t accessory_key[32];
static uint8_t controller_key[32]; /* Public per-test data, never device secrets. */
static int run_result, expected_count;
static bool fail_store;
static int ephemeral_listen(uint8_t family, uint16_t port)
{ (void)port; return rtcp_net_linux.listen(family, 0); }
static int failed_listen(uint8_t family, uint16_t port)
{ (void)family; (void)port; return RTCP_EIO; }
static int failed_udp_open(uint8_t family, uint16_t port, bool reuse)
{ (void)family; (void)port; (void)reuse; return RTCP_EIO; }
static bool failed_random(uint8_t *out, size_t size)
{ (void)out; (void)size; return false; }
static void state_changed(const rtcp_receiver_state_t *state, void *context)
{
    (void)context;
    assert(!state->audio_ready);
    atomic_fetch_add(&state_events, 1);
}
static void ready(uint16_t port, const uint8_t key[32], void *context)
{
    (void)context;
    assert(port);
    rtcp_headless_result_t ignored;
    assert(rtcp_headless_run(&config, &stop_requested, &ignored) == RTCP_ESTATE);
    size_t count;
    assert(airplay_pairing_store_count(&count) == RTCP_OK && count == (size_t)expected_count);
    if (expected_count) {
        uint8_t found[32];
        assert(airplay_pairing_store_lookup_controller((const uint8_t *)"test-controller", 15, found, NULL));
        assert(!memcmp(found, controller_key, 32));
    }
    memcpy(accessory_key, key, 32);
    if (stop_on_ready) {
        atomic_store(&stop_requested, true);
        return;
    }
    if (fail_store) {
        assert(!airplay_pairing_store_save_pairing((const uint8_t *)"new-controller", 14, key, NULL));
        return; /* The poll loop must propagate the failed persistence. */
    }
    assert(airplay_pairing_store_save_pairing((const uint8_t *)"test-controller", 15, controller_key, NULL));
    atomic_store_explicit(&ready_port, port, memory_order_release);
}
static void *run(void *unused)
{ (void)unused; run_result = rtcp_headless_run(&config, &stop_requested, &result); return NULL; }
static void check_store_retired(void)
{
    size_t count;
    assert(airplay_pairing_store_count(&count) == RTCP_ESTATE);
}
static void write_bytes(const char *path, const void *bytes, size_t size)
{
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(bytes, 1, size, file) == size && fclose(file) == 0);
}
static void check_discovery(uint16_t rtsp_port)
{
    mdns_browser_t *browser = mdns_browser_create("_airplay._tcp.local"); assert(browser);
    uint8_t packet[1500];
    size_t length = mdns_browser_build_query(browser, packet, sizeof(packet)); assert(length);
    int fd = rtcp_net_linux.udp_open(RTCP_AF_INET, 0, false); assert(fd >= 0);
    rtcp_addr_t destination = {.family = RTCP_AF_INET, .port = 5353, .addr = {127,0,0,1}};
    assert(rtcp_net_linux.udp_sendto(fd, &destination, packet, length) == (int)length);
    uint64_t deadline = rtcp_os_linux.now_ms() + 5000;
    const mdns_browser_endpoint_t *endpoint = NULL;
    while (rtcp_os_linux.now_ms() < deadline) {
        struct rtcp_pollfd wait = {.fd = fd, .events = RTCP_POLLIN};
        if (rtcp_net_linux.poll(&wait, 1, 100) <= 0) continue;
        rtcp_addr_t from;
        int got = rtcp_net_linux.udp_recvfrom(fd, packet, sizeof(packet), &from);
        assert(got > 0);
        assert(mdns_browser_handle_packet(browser, packet, (size_t)got, from.addr,
                                           (uint32_t)rtcp_os_linux.now_ms()) >= 0);
        endpoint = mdns_browser_best(browser, (uint32_t)rtcp_os_linux.now_ms());
        if (endpoint != NULL && endpoint->port == rtsp_port) break;
    }
    assert(endpoint && endpoint->port == rtsp_port);
    assert(!strcmp(endpoint->instance, "headless test._airplay._tcp.local"));
    assert(endpoint->ipv4[0] == 127 && endpoint->ipv4[3] == 1);
    rtcp_net_linux.close(fd); mdns_browser_destroy(browser);
}

static void filesystem_scenario(void)
{
    char directory[] = "/tmp/rtcp-headless-XXXXXX";
    assert(mkdtemp(directory));
    char seed_path[256], store_path[256], missing_path[256];
    assert(snprintf(seed_path, sizeof(seed_path), "%s/seed", directory) > 0);
    assert(snprintf(store_path, sizeof(store_path), "%s/controllers", directory) > 0);
    assert(snprintf(missing_path, sizeof(missing_path), "%s/absent/file", directory) > 0);
    assert(rtcp_os_linux.random(controller_key, sizeof(controller_key)));
    struct rtcp_os os = rtcp_os_linux;
    struct rtcp_net net = rtcp_net_linux; net.listen = ephemeral_listen;
    config = (rtcp_headless_config_t){.os = &os, .net = &net, .name = "Headless test",
        .device_id = "02:00:00:00:00:01", .width = 1024, .height = 600,
        .seed_path = seed_path, .store_path = store_path, .on_state = state_changed, .on_ready = ready};
    assert(rtcp_headless_run(NULL, &stop_requested, &result) == RTCP_EINVAL);
    os.random = NULL;
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_EINVAL);
    os.random = failed_random;
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_EIO);
    assert(access(seed_path, F_OK) != 0);
    os = rtcp_os_linux;
    config.device_id = "xx:00:00:00:00:01";
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_EINVAL);
    config.device_id = "02:00:00:00:00:01";
    atomic_store(&stop_requested, true);
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_OK);
    assert(access(seed_path, F_OK) != 0); /* Pre-stopped call performs no startup. */
    atomic_store(&stop_requested, false);
    config.seed_path = missing_path;
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_EIO);
    config.seed_path = seed_path;
    uint8_t first_key[32];
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        expected_count = cycle ? 1 : 0;
        config.advertise = cycle == 1;
        config.host_name = "headless-test"; config.ipv4[0] = 127; config.ipv4[3] = 1;
        atomic_store(&ready_port, 0);
        atomic_store(&stop_requested, false);
        pthread_t worker;
        assert(pthread_create(&worker, NULL, run, NULL) == 0);
        uint64_t deadline = rtcp_os_linux.now_ms() + 5000;
        while (!atomic_load_explicit(&ready_port, memory_order_acquire) && rtcp_os_linux.now_ms() < deadline)
            rtcp_os_linux.sleep_ms(1);
        uint16_t port = (uint16_t)atomic_load_explicit(&ready_port, memory_order_acquire);
        assert(port);
        if (cycle == 0) memcpy(first_key, accessory_key, 32);
        else assert(!memcmp(first_key, accessory_key, 32));
        if (config.advertise) check_discovery(port);
        rtcp_addr_t address = {.family = RTCP_AF_INET, .addr = {127,0,0,1}, .port = port};
        int fd = rtcp_net_linux.connect(&address); assert(fd >= 0);
        static const char request[] = "GET /info RTSP/1.0\r\nCSeq: 1\r\n\r\n";
        assert(rtcp_net_linux.send(fd, request, sizeof(request)-1) == (int)sizeof(request)-1);
        char reply[8192]; size_t used = 0;
        const char *body = NULL;
        deadline = rtcp_os_linux.now_ms() + 5000;
        while (rtcp_os_linux.now_ms() < deadline) {
            struct rtcp_pollfd wait = {.fd = fd, .events = RTCP_POLLIN};
            if (rtcp_net_linux.poll(&wait, 1, 100) <= 0) continue;
            int got = rtcp_net_linux.recv(fd, reply + used, sizeof(reply)-1-used);
            assert(got > 0); used += (size_t)got; reply[used] = 0;
            char *header_end = strstr(reply, "\r\n\r\n");
            char *length = strstr(reply, "Content-Length: ");
            if (header_end && length && used >= (size_t)(header_end + 4 - reply) + strtoul(length + 16, NULL, 10)) {
                body = header_end + 4; break;
            }
            assert(used < sizeof(reply)-1);
        }
        assert(body && strstr(reply, "200 OK") && !memcmp(body, "bplist00", 8));
        airplay_bplist_doc_t plist;
        assert(airplay_bplist_open((const uint8_t *)body, used-(size_t)(body-reply), &plist) == AIRPLAY_BPLIST_OK);
        uint16_t width, height;
        assert(airplay_bplist_get_main_display_size(&plist, &width, &height) == AIRPLAY_BPLIST_OK);
        assert(width == 1024 && height == 600);
        atomic_store_explicit(&stop_requested, true, memory_order_release);
        assert(pthread_join(worker, NULL) == 0 && run_result == RTCP_OK);
        rtcp_net_linux.close(fd);
        check_store_retired();
        uint8_t seed[32]; FILE *file = fopen(seed_path, "rb"); assert(file);
        assert(fread(seed, 1, 32, file) == 32 && fclose(file) == 0);
        airplay_ed25519_keypair_t key;
        assert(airplay_ed25519_keypair_from_seed(seed, &key));
        assert(!memcmp(key.public_key, accessory_key, 32));
        airplay_ed25519_keypair_clear(&key);
    }
    /* Fail after moving an existing primary to backup; rollback must retain
     * the original controller table on both POSIX and FatFS semantics. */
    atomic_store(&stop_requested, false); config.advertise = false;
    expected_count = 1; fail_store = true; fail_commit = true;
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_EIO);
    assert(!fail_commit); check_store_retired(); fail_store = false;
    stop_on_ready = true;
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_OK);
    char seed_backup[260], store_backup[260];
    snprintf(seed_backup, sizeof(seed_backup), "%s.bak", seed_path);
    snprintf(store_backup, sizeof(store_backup), "%s.bak", store_path);
    (void)remove(seed_backup); (void)remove(store_backup);
    assert(rename(seed_path, seed_backup) == 0 && rename(store_path, store_backup) == 0);
    atomic_store(&stop_requested, false);
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_OK);
    assert(!memcmp(first_key, accessory_key, 32)); /* Never generate over a recoverable identity. */
    assert(rename(seed_backup, seed_path) == 0 && rename(store_backup, store_path) == 0);
    stop_on_ready = false;
    assert(atomic_load(&state_events) >= 9);
    atomic_store(&stop_requested, false);
    os.random = failed_random;
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_EIO);
    os = rtcp_os_linux;
    config.advertise = false;
    net.listen = failed_listen;
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_EIO);
    check_store_retired();
    net.listen = ephemeral_listen;
    config.advertise = true; config.host_name = "headless-test";
    config.ipv4[0] = 127; config.ipv4[3] = 1; net.udp_open = failed_udp_open;
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_EIO);
    check_store_retired();
    config.advertise = false;
    write_bytes(store_path, "corrupt", 7);
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_EIO);
    assert(unlink(store_path) == 0);
    config.store_path = missing_path; expected_count = 0; fail_store = true;
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_EIO);
    check_store_retired();
    config.store_path = store_path; fail_store = false;
    write_bytes(seed_path, "bad", 3);
    assert(rtcp_headless_run(&config, &stop_requested, &result) == RTCP_EIO);
    FILE *file = fopen(seed_path, "rb"); char corrupt[4] = {0}; assert(file);
    assert(fread(corrupt, 1, 4, file) == 3 && fclose(file) == 0 && !strcmp(corrupt, "bad"));
    (void)remove(seed_backup); (void)remove(store_backup);
    assert(unlink(seed_path) == 0 && rmdir(directory) == 0);
    puts("Generic Headless: real TCP/info and UDP/mDNS, secure identity, persistent controller records, 3 restarts, stop/join and failure rollback passed");
}

int main(void)
{
    for (filesystem_mode = 0; filesystem_mode < 3; ++filesystem_mode) {
        atomic_store(&stop_requested, false); atomic_store(&ready_port, 0);
        atomic_store(&state_events, 0); expected_count = 0;
        fail_store = fail_commit = stop_on_ready = false;
        filesystem_scenario();
    }
    puts("Headless filesystem: POSIX/negative errno/FatFS no-overwrite, commit rollback and backup recovery passed");
}
