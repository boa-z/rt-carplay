/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtcp_linux.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

typedef struct {
    rtcp_mutex_t mutex;
    int value;
    int done;
} shared_t;

static void worker(void *arg)
{
    shared_t *shared = arg;
    rtcp_os_linux.mutex_lock(shared->mutex);
    shared->value = 42;
    shared->done = 1;
    rtcp_os_linux.mutex_unlock(shared->mutex);
}

int main(void)
{
    /* Clock and sleep. */
    uint64_t before = rtcp_os_linux.now_ms();
    rtcp_os_linux.sleep_ms(10);
    uint64_t after = rtcp_os_linux.now_ms();
    assert(after >= before);
    assert(after - before >= 5U);

    /* Heap. */
    int *value = rtcp_os_linux.alloc(sizeof(*value));
    assert(value != NULL);
    *value = 7;
    assert(*value == 7);
    rtcp_os_linux.free(value);

    /* Mutex + detached thread. */
    shared_t shared = {0};
    shared.mutex = rtcp_os_linux.mutex_create();
    assert(shared.mutex != NULL);
    assert(rtcp_os_linux.thread_create("worker", worker, &shared, 4096, 5) != NULL);
    unsigned spins = 0U;
    for (;;) {
        rtcp_os_linux.mutex_lock(shared.mutex);
        int done = shared.done;
        rtcp_os_linux.mutex_unlock(shared.mutex);
        if (done) break;
        assert(++spins < 2000U);
        rtcp_os_linux.sleep_ms(1);
    }
    assert(shared.value == 42);
    rtcp_os_linux.mutex_destroy(shared.mutex);

    /* TCP loopback. */
    int listener = rtcp_net_linux.listen(0);
    assert(listener >= 0);
    uint16_t port = rtcp_net_linux_local_port(listener);
    assert(port != 0U);

    rtcp_addr_t peer = {
        .family = RTCP_AF_INET,
        .addr = {127, 0, 0, 1},
        .port = port,
    };
    int client = rtcp_net_linux.connect(&peer);
    assert(client >= 0);
    int server = rtcp_net_linux.accept(listener, &peer);
    assert(server >= 0);
    assert(peer.family == RTCP_AF_INET && peer.port != 0U);

    static const char ping[] = "ping";
    assert(rtcp_net_linux.send(client, ping, sizeof(ping) - 1U) ==
           (int)(sizeof(ping) - 1U));

    struct rtcp_pollfd pollfd = {
        .fd = server,
        .events = RTCP_POLLIN,
        .revents = 0,
    };
    assert(rtcp_net_linux.poll(&pollfd, 1, 1000) == 1);
    assert(pollfd.revents & RTCP_POLLIN);

    char buffer[16] = {0};
    int received = rtcp_net_linux.recv(server, buffer, sizeof(buffer) - 1U);
    assert(received == (int)(sizeof(ping) - 1U));
    assert(memcmp(buffer, ping, sizeof(ping) - 1U) == 0);

    static const char pong[] = "pong";
    assert(rtcp_net_linux.send(server, pong, sizeof(pong) - 1U) ==
           (int)(sizeof(pong) - 1U));
    memset(buffer, 0, sizeof(buffer));
    received = rtcp_net_linux.recv(client, buffer, sizeof(buffer) - 1U);
    assert(received == (int)(sizeof(pong) - 1U));
    assert(memcmp(buffer, pong, sizeof(pong) - 1U) == 0);

    /* Argument validation. */
    assert(rtcp_net_linux.listen(0) >= 0);
    assert(rtcp_net_linux.send(-1, ping, 1) == RTCP_EINVAL);
    assert(rtcp_net_linux.poll(NULL, 1, 0) == RTCP_EINVAL);
    assert(rtcp_net_linux.connect(NULL) == RTCP_EINVAL);

    /* Documented stubs. */
    uint8_t bdaddr[6] = {0};
    assert(rtcp_bt_linux.enable("linux", bdaddr) == RTCP_ENOTSUP);
    assert(rtcp_wifi_linux.ap_start("ssid", "pass", 6) == RTCP_ENOTSUP);

    rtcp_net_linux.close(client);
    rtcp_net_linux.close(server);
    rtcp_net_linux.close(listener);

    puts("linux port host test passed");
    return 0;
}
