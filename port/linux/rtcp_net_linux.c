/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1 /* struct ip_mreq, SO_REUSEPORT */
#include "rtcp_linux.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rtcp/rtcp.h"

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
        close(fd);
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
        } else if (storage.ss_family == AF_INET6) {
            const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)&storage;
            peer->family = RTCP_AF_INET6;
            memcpy(peer->addr, &in6->sin6_addr, 16U);
            peer->port = ntohs(in6->sin6_port);
        }
    }
    return fd;
}

static int connect_socket(const rtcp_addr_t *addr)
{
    if (addr == NULL) return RTCP_EINVAL;
    int fd = socket(addr->family == RTCP_AF_INET6 ? AF_INET6 : AF_INET,
                    SOCK_STREAM, 0);
    if (fd < 0) return RTCP_EIO;

    int rc;
    if (addr->family == RTCP_AF_INET6) {
        struct sockaddr_in6 in6;
        memset(&in6, 0, sizeof(in6));
        in6.sin6_family = AF_INET6;
        memcpy(&in6.sin6_addr, addr->addr, 16U);
        in6.sin6_port = htons(addr->port);
        rc = connect(fd, (struct sockaddr *)&in6, sizeof(in6));
    } else {
        struct sockaddr_in in;
        memset(&in, 0, sizeof(in));
        in.sin_family = AF_INET;
        memcpy(&in.sin_addr, addr->addr, 4U);
        in.sin_port = htons(addr->port);
        rc = connect(fd, (struct sockaddr *)&in, sizeof(in));
    }
    if (rc != 0) {
        close(fd);
        return RTCP_EIO;
    }
    return fd;
}

static int send_bytes(int fd, const void *buf, size_t len)
{
    if (fd < 0 || (buf == NULL && len != 0U)) return RTCP_EINVAL;
#ifdef MSG_NOSIGNAL
    ssize_t rc = send(fd, buf, len, MSG_NOSIGNAL);
#else
    ssize_t rc = send(fd, buf, len, 0);
#endif
    if (rc < 0) return RTCP_EIO;
    return (int)rc;
}

static int recv_bytes(int fd, void *buf, size_t len)
{
    if (fd < 0 || (buf == NULL && len != 0U)) return RTCP_EINVAL;
    ssize_t rc = recv(fd, buf, len, 0);
    if (rc < 0) return RTCP_EIO;
    return (int)rc; /* 0 means peer closed */
}

static int poll_fds(struct rtcp_pollfd *fds, int n, int timeout_ms)
{
    if (fds == NULL || n < 0) return RTCP_EINVAL;
    struct pollfd local[8];
    if (n > (int)(sizeof(local) / sizeof(local[0]))) return RTCP_EINVAL;
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
    if (fd >= 0) close(fd);
}

static int udp_open(uint16_t port, bool reuse)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return RTCP_EIO;

    if (reuse) {
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return RTCP_EIO;
    }
    return fd;
}

static int udp_join(int fd, const uint8_t group[4])
{
    if (fd < 0 || group == NULL) return RTCP_EINVAL;
    struct ip_mreq request;
    memset(&request, 0, sizeof(request));
    memcpy(&request.imr_multiaddr.s_addr, group, 4U);
    request.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request,
                   sizeof(request)) != 0) {
        return RTCP_EIO;
    }
    return RTCP_OK;
}

static int udp_sendto(int fd, const rtcp_addr_t *to, const void *buf, size_t len)
{
    if (fd < 0 || to == NULL || (buf == NULL && len != 0U)) return RTCP_EINVAL;
    if (to->family != RTCP_AF_INET) return RTCP_ENOTSUP;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    memcpy(&addr.sin_addr, to->addr, 4U);
    addr.sin_port = htons(to->port);
    ssize_t rc = sendto(fd, buf, len, 0, (struct sockaddr *)&addr, sizeof(addr));
    return rc < 0 ? RTCP_EIO : (int)rc;
}

static int udp_recvfrom(int fd, void *buf, size_t len, rtcp_addr_t *from)
{
    if (fd < 0 || (buf == NULL && len != 0U)) return RTCP_EINVAL;
    struct sockaddr_storage storage;
    socklen_t storage_len = sizeof(storage);
    ssize_t rc = recvfrom(fd, buf, len, 0, (struct sockaddr *)&storage,
                          &storage_len);
    if (rc < 0) return RTCP_EIO;
    if (from != NULL) {
        memset(from, 0, sizeof(*from));
        if (storage.ss_family == AF_INET) {
            const struct sockaddr_in *in = (const struct sockaddr_in *)&storage;
            from->family = RTCP_AF_INET;
            memcpy(from->addr, &in->sin_addr, 4U);
            from->port = ntohs(in->sin_port);
        } else if (storage.ss_family == AF_INET6) {
            const struct sockaddr_in6 *in6 =
                (const struct sockaddr_in6 *)&storage;
            from->family = RTCP_AF_INET6;
            memcpy(from->addr, &in6->sin6_addr, 16U);
            from->port = ntohs(in6->sin6_port);
        }
    }
    return (int)rc;
}

uint16_t rtcp_net_linux_local_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&addr, &len) != 0) return 0U;
    return ntohs(addr.sin_port);
}

const struct rtcp_net rtcp_net_linux = {
    .listen = listen_socket,
    .accept = accept_socket,
    .connect = connect_socket,
    .send = send_bytes,
    .recv = recv_bytes,
    .local_port = rtcp_net_linux_local_port,
    .poll = poll_fds,
    .close = close_fd,
    .udp_open = udp_open,
    .udp_join = udp_join,
    .udp_sendto = udp_sendto,
    .udp_recvfrom = udp_recvfrom,
};
