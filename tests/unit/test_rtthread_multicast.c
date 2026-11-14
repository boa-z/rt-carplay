/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "rtcp_rtthread.h"
#include "rtcp/rtcp.h"

int main(void)
{
    const uint8_t loopback[4] = {127, 0, 0, 1};
    const uint8_t group[4] = {224, 0, 0, 251};
    int fd = rtcp_net_rtthread.udp_open(RTCP_AF_INET, 0, true);
    assert(fd >= 0);
    rtcp_net_rtthread_multicast_interface(loopback);
    assert(rtcp_net_rtthread.udp_join(fd, group) == RTCP_OK);
    struct in_addr selected;
    socklen_t len = sizeof(selected);
    assert(!getsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &selected, &len));
    assert(!memcmp(&selected, loopback, 4));
    rtcp_net_rtthread.close(fd);

    rtcp_net_rtthread_multicast_interface(NULL);
    fd = rtcp_net_rtthread.udp_open(RTCP_AF_INET, 0, true);
    assert(fd >= 0);
    assert(rtcp_net_rtthread.udp_join(fd, group) == RTCP_OK);
    assert(!getsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &selected, &len));
    assert(selected.s_addr == INADDR_ANY);
    rtcp_net_rtthread.close(fd);
    assert(rtcp_net_rtthread.udp_join(fd, group) == RTCP_EIO);
    puts("RT-Thread network port: explicit multicast interface, reset and closed-socket failure passed");
    return 0;
}
