/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_timing_port.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "airplay_timing.h"
#include "rtcp/rtcp.h"
#include "rtcp_linux.h"

#define WAIT_MS 500

static rtcp_addr_t loopback(uint16_t port)
{
    rtcp_addr_t address = {0};
    address.family = RTCP_AF_INET;
    address.addr[0] = 127U;
    address.addr[3] = 1U;
    address.port = port;
    return address;
}

/* The net seam's recv is blocking; only read after poll reports data. */
static int wait_readable(const struct rtcp_net *net, int fd)
{
    struct rtcp_pollfd pfd = {.fd = fd, .events = RTCP_POLLIN};
    int ready = net->poll(&pfd, 1, WAIT_MS);
    return ready > 0 && (pfd.revents & RTCP_POLLIN);
}

static int udp_send_all(const struct rtcp_net *net, int fd,
                        const rtcp_addr_t *to, const uint8_t *bytes,
                        size_t len)
{
    return net->udp_sendto(fd, to, bytes, len) == (int)len;
}

static void test_responds_to_request(void)
{
    airplay_timing_port_t *port = NULL;
    airplay_timing_port_config_t config = {0};
    config.peer = loopback(0U); /* no periodic requests */
    assert(airplay_timing_port_start(&rtcp_os_linux, &rtcp_net_linux, &config,
                                     &port) == RTCP_OK);
    assert(port != NULL);
    assert(airplay_timing_port_get_port(port) != 0U);
    assert(airplay_timing_port_get_fd(port) >= 0);

    int client = rtcp_net_linux.udp_open(0U, false);
    assert(client >= 0);
    uint16_t client_port = rtcp_net_linux.local_port(client);
    assert(client_port != 0U);

    uint8_t request[AIRPLAY_TIMING_PACKET_BYTES];
    size_t request_len = 0U;
    assert(airplay_timing_build_request(UINT64_C(0x0102030405060708), request,
                                        sizeof(request), &request_len));
    rtcp_addr_t server = loopback(airplay_timing_port_get_port(port));
    assert(udp_send_all(&rtcp_net_linux, client, &server, request,
                        request_len));

    assert(wait_readable(&rtcp_net_linux, airplay_timing_port_get_fd(port)));
    airplay_timing_port_poll(port, true);

    assert(wait_readable(&rtcp_net_linux, client));
    uint8_t response[AIRPLAY_TIMING_PACKET_BYTES + 1U];
    rtcp_addr_t from = {0};
    int received = rtcp_net_linux.udp_recvfrom(client, response,
                                               sizeof(response), &from);
    assert(received == (int)AIRPLAY_TIMING_PACKET_BYTES);
    assert(response[1] == AIRPLAY_TIMING_PT_RESPONSE);
    /* The response echoes the request's transmit timestamp as its origin. */
    assert(memcmp(response + 8U, request + 24U, 8U) == 0);

    rtcp_net_linux.close(client);
    airplay_timing_port_stop(port, &rtcp_net_linux);
}

static void test_periodic_requests(void)
{
    int client = rtcp_net_linux.udp_open(0U, false);
    assert(client >= 0);
    uint16_t client_port = rtcp_net_linux.local_port(client);
    assert(client_port != 0U);

    airplay_timing_port_t *port = NULL;
    airplay_timing_port_config_t config = {0};
    config.peer = loopback(client_port);
    assert(airplay_timing_port_start(&rtcp_os_linux, &rtcp_net_linux, &config,
                                     &port) == RTCP_OK);

    /* The first poll is due immediately. */
    airplay_timing_port_poll(port, false);
    assert(wait_readable(&rtcp_net_linux, client));
    uint8_t request[AIRPLAY_TIMING_PACKET_BYTES + 1U];
    rtcp_addr_t from = {0};
    int received = rtcp_net_linux.udp_recvfrom(client, request,
                                               sizeof(request), &from);
    assert(received == (int)AIRPLAY_TIMING_PACKET_BYTES);
    assert(request[1] == AIRPLAY_TIMING_PT_REQUEST);

    /* Answer so the pending exchange completes. */
    uint8_t response[AIRPLAY_TIMING_PACKET_BYTES];
    size_t response_len = 0U;
    assert(airplay_timing_build_response(request, AIRPLAY_TIMING_PACKET_BYTES,
                                         UINT64_C(1), UINT64_C(2), response,
                                         sizeof(response), &response_len));
    rtcp_addr_t server = loopback(airplay_timing_port_get_port(port));
    assert(udp_send_all(&rtcp_net_linux, client, &server, response,
                        response_len));

    assert(wait_readable(&rtcp_net_linux, airplay_timing_port_get_fd(port)));
    airplay_timing_port_poll(port, true);

    /* The next request is a full interval away: not due yet. */
    airplay_timing_port_poll(port, false);
    struct rtcp_pollfd pfd = {.fd = client, .events = RTCP_POLLIN};
    assert(rtcp_net_linux.poll(&pfd, 1, 50) == 0);

    rtcp_net_linux.close(client);
    airplay_timing_port_stop(port, &rtcp_net_linux);
}

static void test_argument_checks(void)
{
    airplay_timing_port_config_t config = {0};
    config.peer = loopback(1U);
    airplay_timing_port_t *port = NULL;
    assert(airplay_timing_port_start(NULL, &rtcp_net_linux, &config, &port) ==
           RTCP_EINVAL);
    assert(airplay_timing_port_start(&rtcp_os_linux, NULL, &config, &port) ==
           RTCP_EINVAL);
    config.peer.family = 0U;
    assert(airplay_timing_port_start(&rtcp_os_linux, &rtcp_net_linux, &config,
                                     &port) == RTCP_EINVAL);
    assert(airplay_timing_port_get_port(NULL) == 0U);
    assert(airplay_timing_port_get_fd(NULL) == -1);
    airplay_timing_port_poll(NULL, true);
    airplay_timing_port_stop(NULL, &rtcp_net_linux);
}

int main(void)
{
    test_responds_to_request();
    test_periodic_requests();
    test_argument_checks();
    puts("airplay timing port host test passed");
    return 0;
}
