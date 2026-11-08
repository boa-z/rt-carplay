/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * RT-Thread TCP seam.  Uses the BSD socket API exposed by SAL (or lwIP
 * directly) so the same code works with the ArtInChip SDK network stack.
 */
#include "rtcp_rtthread.h"

#include <string.h>
#include <rtthread.h>

#ifdef RT_USING_SAL
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#else
#include <lwip/sockets.h>
#endif

#include "rtcp/rtcp.h"

/* Upper bound for one poll() batch; stack cost is sizeof(struct pollfd) each. */
#ifndef RTCP_NET_POLL_MAX
#define RTCP_NET_POLL_MAX 8
#endif

static int listen_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return RTCP_EIO;

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 4) != 0) {
        closesocket(fd);
        return RTCP_EIO;
    }
    return fd;
}

static int accept_socket(int lfd, rtcp_addr_t *peer)
{
    struct sockaddr_storage storage;
    socklen_t len = sizeof(storage);
    int fd = accept(lfd, (struct sockaddr *)&storage, &len);
    if (fd < 0) return RTCP_EIO;
    if (peer != NULL) {
        memset(peer, 0, sizeof(*peer));
        if (storage.ss_family == AF_INET) {
            const struct sockaddr_in *in = (const struct sockaddr_in *)&storage;
            peer->family = RTCP_AF_INET;
            memcpy(peer->addr, &in->sin_addr, 4U);
            peer->port = ntohs(in->sin_port);
        }
    }
    return fd;
}

static int connect_socket(const rtcp_addr_t *addr)
{
    if (addr == NULL || addr->family != RTCP_AF_INET) return RTCP_EINVAL;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return RTCP_EIO;

    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    memcpy(&in.sin_addr, addr->addr, 4U);
    in.sin_port = htons(addr->port);
    if (connect(fd, (struct sockaddr *)&in, sizeof(in)) != 0) {
        closesocket(fd);
        return RTCP_EIO;
    }
    return fd;
}

static int send_bytes(int fd, const void *buf, size_t len)
{
    if (fd < 0 || (buf == NULL && len != 0U)) return RTCP_EINVAL;
    int rc = send(fd, buf, len, 0);
    return rc < 0 ? RTCP_EIO : rc;
}

static int recv_bytes(int fd, void *buf, size_t len)
{
    if (fd < 0 || (buf == NULL && len != 0U)) return RTCP_EINVAL;
    int rc = recv(fd, buf, len, 0);
    return rc < 0 ? RTCP_EIO : rc;
}

static int poll_fds(struct rtcp_pollfd *fds, int n, int timeout_ms)
{
    if (fds == NULL || n < 0 || n > RTCP_NET_POLL_MAX) return RTCP_EINVAL;
    struct pollfd local[RTCP_NET_POLL_MAX];
    for (int i = 0; i < n; ++i) {
        local[i].fd = fds[i].fd;
        local[i].events = 0;
        if (fds[i].events & RTCP_POLLIN) local[i].events |= POLLIN;
        if (fds[i].events & RTCP_POLLOUT) local[i].events |= POLLOUT;
        local[i].revents = 0;
    }
    int rc = poll(local, (nfds_t)n, timeout_ms);
    if (rc < 0) return RTCP_EIO;
    for (int i = 0; i < n; ++i) {
        fds[i].revents = 0;
        if (local[i].revents & POLLIN) fds[i].revents |= RTCP_POLLIN;
        if (local[i].revents & POLLOUT) fds[i].revents |= RTCP_POLLOUT;
        if (local[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fds[i].revents |= RTCP_POLLERR;
        }
    }
    return rc;
}

static void close_fd(int fd)
{
    if (fd >= 0) closesocket(fd);
}

uint16_t rtcp_net_rtthread_local_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&addr, &len) != 0) return 0U;
    return ntohs(addr.sin_port);
}

const struct rtcp_net rtcp_net_rtthread = {
    .listen = listen_socket,
    .accept = accept_socket,
    .connect = connect_socket,
    .send = send_bytes,
    .recv = recv_bytes,
    .poll = poll_fds,
    .close = close_fd,
};
