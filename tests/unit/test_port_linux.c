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

    /* CSPRNG. */
    assert(rtcp_os_linux.random != NULL);
    uint8_t random_a[32] = {0};
    uint8_t random_b[32] = {0};
    assert(rtcp_os_linux.random(random_a, sizeof(random_a)));
    assert(rtcp_os_linux.random(random_b, sizeof(random_b)));
    assert(memcmp(random_a, random_b, sizeof(random_a)) != 0);

    /* TCP loopback. */
    int listener = rtcp_net_linux.listen(RTCP_AF_INET, 0);
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
    assert(rtcp_net_linux.listen(RTCP_AF_INET, 0) >= 0);
    assert(rtcp_net_linux.listen(9U, 0) == RTCP_EINVAL);
    assert(rtcp_net_linux.send(-1, ping, 1) == RTCP_EINVAL);
    assert(rtcp_net_linux.poll(NULL, 1, 0) == RTCP_EINVAL);
    assert(rtcp_net_linux.connect(NULL) == RTCP_EINVAL);

    /* IPv6 loopback, the wired CarPlay transport.  Skip only when the host
     * has no IPv6 loopback at all. */
    int listener6 = rtcp_net_linux.listen(RTCP_AF_INET6, 0);
    if (listener6 >= 0) {
        uint16_t port6 = rtcp_net_linux_local_port(listener6);
        assert(port6 != 0U);
        rtcp_addr_t loopback6 = {
            .family = RTCP_AF_INET6,
            .addr = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
            .port = port6,
        };
        int client6 = rtcp_net_linux.connect(&loopback6);
        assert(client6 >= 0);
        rtcp_addr_t peer6 = {0};
        int server6 = rtcp_net_linux.accept(listener6, &peer6);
        assert(server6 >= 0);
        assert(peer6.family == RTCP_AF_INET6 && peer6.port != 0U);
        assert(rtcp_net_linux.send(client6, ping, sizeof(ping) - 1U) ==
               (int)(sizeof(ping) - 1U));
        struct rtcp_pollfd poll6 = {
            .fd = server6,
            .events = RTCP_POLLIN,
            .revents = 0,
        };
        assert(rtcp_net_linux.poll(&poll6, 1, 1000) == 1);
        memset(buffer, 0, sizeof(buffer));
        assert(rtcp_net_linux.recv(server6, buffer, sizeof(buffer) - 1U) ==
               (int)(sizeof(ping) - 1U));
        assert(memcmp(buffer, ping, sizeof(ping) - 1U) == 0);
        rtcp_net_linux.close(client6);
        rtcp_net_linux.close(server6);
        rtcp_net_linux.close(listener6);

        int receiver6 = rtcp_net_linux.udp_open(RTCP_AF_INET6, 0U, false);
        assert(receiver6 >= 0);
        uint16_t udp_port6 = rtcp_net_linux_local_port(receiver6);
        assert(udp_port6 != 0U);
        int sender6 = rtcp_net_linux.udp_open(RTCP_AF_INET6, 0U, false);
        assert(sender6 >= 0);
        rtcp_addr_t unicast6 = {
            .family = RTCP_AF_INET6,
            .addr = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
            .port = udp_port6,
        };
        static const char datagram6[] = "wired";
        assert(rtcp_net_linux.udp_sendto(sender6, &unicast6, datagram6,
                                         sizeof(datagram6)) ==
               (int)sizeof(datagram6));
        struct rtcp_pollfd poll6_udp = {
            .fd = receiver6,
            .events = RTCP_POLLIN,
            .revents = 0,
        };
        assert(rtcp_net_linux.poll(&poll6_udp, 1, 1000) == 1);
        rtcp_addr_t source6 = {0};
        assert(rtcp_net_linux.udp_recvfrom(receiver6, buffer, sizeof(buffer),
                                           &source6) ==
               (int)sizeof(datagram6));
        assert(source6.family == RTCP_AF_INET6);
        rtcp_net_linux.close(receiver6);
        rtcp_net_linux.close(sender6);
    } else {
        puts("IPv6 loopback unavailable; skipped");
    }

    /* UDP loopback (mDNS transport).  Multicast join is environment
     * dependent and is exercised on the target, not here. */
    int receiver = rtcp_net_linux.udp_open(RTCP_AF_INET, 0U, false);
    assert(receiver >= 0);
    uint16_t udp_port = rtcp_net_linux_local_port(receiver);
    assert(udp_port != 0U);
    int sender = rtcp_net_linux.udp_open(RTCP_AF_INET, 0U, false);
    assert(sender >= 0);

    rtcp_addr_t unicast = {
        .family = RTCP_AF_INET,
        .addr = {127, 0, 0, 1},
        .port = udp_port,
    };
    static const char datagram[] = "mDNS";
    assert(rtcp_net_linux.udp_sendto(sender, &unicast, datagram,
                                     sizeof(datagram)) ==
           (int)sizeof(datagram));

    struct rtcp_pollfd datagram_poll = {
        .fd = receiver,
        .events = RTCP_POLLIN,
        .revents = 0,
    };
    assert(rtcp_net_linux.poll(&datagram_poll, 1, 1000) == 1);
    assert(datagram_poll.revents & RTCP_POLLIN);

    rtcp_addr_t source = {0};
    memset(buffer, 0, sizeof(buffer));
    int datagram_len = rtcp_net_linux.udp_recvfrom(receiver, buffer,
                                                   sizeof(buffer), &source);
    assert(datagram_len == (int)sizeof(datagram));
    assert(memcmp(buffer, datagram, sizeof(datagram)) == 0);
    assert(source.family == RTCP_AF_INET);
    assert(source.addr[0] == 127U && source.addr[3] == 1U);

    assert(rtcp_net_linux.udp_sendto(-1, &unicast, datagram, 1) ==
           RTCP_EINVAL);
    assert(rtcp_net_linux.udp_recvfrom(-1, buffer, 1, NULL) == RTCP_EINVAL);
    rtcp_net_linux.close(receiver);
    rtcp_net_linux.close(sender);

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
