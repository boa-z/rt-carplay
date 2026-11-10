/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * RT-Thread TCP seam.  Uses the BSD socket API exposed by SAL (or lwIP
 * directly) so the same code works with the ArtInChip SDK network stack.
 * IPv6 support follows the stack build: when neither SAL's NETDEV_IPV6 nor
 * lwIP's LWIP_IPV6 is enabled, AF_INET6 requests return RTCP_ENOTSUP.
 */
#include "rtcp_rtthread.h"

#include <string.h>
#include <rtthread.h>

#ifdef RT_USING_SAL
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <fcntl.h>
#else
#include <lwip/sockets.h>
#include <sys/errno.h>
#endif

#include "rtcp/rtcp.h"

#if (defined(NETDEV_IPV6) && NETDEV_IPV6) || \
    (defined(LWIP_IPV6) && LWIP_IPV6)
#define RTCP_NET_HAVE_IPV6 1
#endif

/* Upper bound for one poll() batch; stack cost is sizeof(struct pollfd) each.
 * The AirPlay server polls RTSP, event, timing, screen and three audio
 * data/control pairs, so 12 leaves a little headroom. */
#ifndef RTCP_NET_POLL_MAX
#define RTCP_NET_POLL_MAX 12
#endif

static int poll_fds(struct rtcp_pollfd *fds, int n, int timeout_ms);

static int fill_sockaddr(const rtcp_addr_t *addr,
                         struct sockaddr_storage *storage, socklen_t *length)
{
    memset(storage, 0, sizeof(*storage));
    if (addr->family == RTCP_AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)storage;
        in->sin_family = AF_INET;
        memcpy(&in->sin_addr, addr->addr, 4U);
        in->sin_port = htons(addr->port);
        *length = sizeof(*in);
        return RTCP_OK;
    }
#if RTCP_NET_HAVE_IPV6
    if (addr->family == RTCP_AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)storage;
        in6->sin6_family = AF_INET6;
        memcpy(&in6->sin6_addr, addr->addr, 16U);
        in6->sin6_port = htons(addr->port);
        in6->sin6_scope_id = addr->scope_id;
        *length = sizeof(*in6);
        return RTCP_OK;
    }
#endif
    return RTCP_ENOTSUP;
}

static void read_sockaddr(const struct sockaddr_storage *storage,
                          rtcp_addr_t *addr)
{
    memset(addr, 0, sizeof(*addr));
    if (storage->ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)storage;
        addr->family = RTCP_AF_INET;
        memcpy(addr->addr, &in->sin_addr, 4U);
        addr->port = ntohs(in->sin_port);
    }
#if RTCP_NET_HAVE_IPV6
    else if (storage->ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)storage;
        addr->family = RTCP_AF_INET6;
        memcpy(addr->addr, &in6->sin6_addr, 16U);
        addr->port = ntohs(in6->sin6_port);
        addr->scope_id = (uint16_t)in6->sin6_scope_id;
    }
#endif
}

static int listen_socket(uint8_t family, uint16_t port)
{
    int domain;
    if (family == RTCP_AF_INET) {
        domain = AF_INET;
    } else if (family == RTCP_AF_INET6) {
#if RTCP_NET_HAVE_IPV6
        domain = AF_INET6;
#else
        return RTCP_ENOTSUP;
#endif
    } else {
        return RTCP_EINVAL;
    }

    int fd = socket(domain, SOCK_STREAM, 0);
    if (fd < 0) return RTCP_EIO;

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (family == RTCP_AF_INET6) {
#if RTCP_NET_HAVE_IPV6
        struct sockaddr_in6 addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(fd, 4) != 0) {
            closesocket(fd);
            return RTCP_EIO;
        }
#endif
    } else {
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
    }
    return fd;
}

static int accept_socket(int lfd, rtcp_addr_t *peer)
{
    struct sockaddr_storage storage;
    socklen_t len = sizeof(storage);
    int fd = accept(lfd, (struct sockaddr *)&storage, &len);
    if (fd < 0) return RTCP_EIO;
    if (peer != NULL) read_sockaddr(&storage, peer);
    return fd;
}

static int connect_socket(const rtcp_addr_t *addr)
{
    if (addr == NULL) return RTCP_EINVAL;
    int domain;
    if (addr->family == RTCP_AF_INET) {
        domain = AF_INET;
    } else if (addr->family == RTCP_AF_INET6) {
#if RTCP_NET_HAVE_IPV6
        domain = AF_INET6;
#else
        return RTCP_ENOTSUP;
#endif
    } else {
        return RTCP_EINVAL;
    }
    struct sockaddr_storage storage;
    socklen_t length = 0;
    int rc = fill_sockaddr(addr, &storage, &length);
    if (rc != RTCP_OK) return rc;

    int fd = socket(domain, SOCK_STREAM, 0);
    if (fd < 0) return RTCP_EIO;
    if (connect(fd, (struct sockaddr *)&storage, length) != 0) {
        closesocket(fd);
        return RTCP_EIO;
    }
    return fd;
}

/* Same socket setup as connect_socket(), but the TCP handshake is bounded by
 * timeout_ms.  A blocking connect() retries SYNs for tens of seconds, which
 * would stall the receiver's single poll thread; the CarPlay Control
 * connect-out runs there. */
static int connect_timeout_socket(const rtcp_addr_t *addr, int timeout_ms)
{
    if (addr == NULL) return RTCP_EINVAL;
    int domain;
    if (addr->family == RTCP_AF_INET) {
        domain = AF_INET;
    } else if (addr->family == RTCP_AF_INET6) {
#if RTCP_NET_HAVE_IPV6
        domain = AF_INET6;
#else
        return RTCP_ENOTSUP;
#endif
    } else {
        return RTCP_EINVAL;
    }
    struct sockaddr_storage storage;
    socklen_t length = 0;
    int rc = fill_sockaddr(addr, &storage, &length);
    if (rc != RTCP_OK) return rc;

    int fd = socket(domain, SOCK_STREAM, 0);
    if (fd < 0) return RTCP_EIO;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        /* No socket-level fcntl on this port: the blocking connect is still
         * correct, just not time-bounded here. */
        if (connect(fd, (struct sockaddr *)&storage, length) != 0) {
            closesocket(fd);
            return RTCP_EIO;
        }
        return fd;
    }

    rc = connect(fd, (struct sockaddr *)&storage, length);
    if (rc != 0) {
        if (errno != EINPROGRESS) {
            closesocket(fd);
            return RTCP_EIO;
        }
        struct rtcp_pollfd descriptor = {
            .fd = fd,
            .events = RTCP_POLLOUT,
            .revents = 0,
        };
        rc = poll_fds(&descriptor, 1, timeout_ms);
        if (rc <= 0) {
            closesocket(fd);
            return rc < 0 ? RTCP_EIO : RTCP_ETIMEDOUT;
        }
        int error = 0;
        socklen_t error_len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) != 0 ||
            error != 0) {
            closesocket(fd);
            return RTCP_EIO;
        }
    }

    /* Callers use the blocking send/recv path afterwards. */
    (void)fcntl(fd, F_SETFL, flags);
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

static int udp_open(uint8_t family, uint16_t port, bool reuse)
{
    int domain;
    if (family == RTCP_AF_INET) {
        domain = AF_INET;
    } else if (family == RTCP_AF_INET6) {
#if RTCP_NET_HAVE_IPV6
        domain = AF_INET6;
#else
        return RTCP_ENOTSUP;
#endif
    } else {
        return RTCP_EINVAL;
    }

    int fd = socket(domain, SOCK_DGRAM, 0);
    if (fd < 0) return RTCP_EIO;

    if (reuse) {
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    }

    if (family == RTCP_AF_INET6) {
#if RTCP_NET_HAVE_IPV6
        struct sockaddr_in6 addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            closesocket(fd);
            return RTCP_EIO;
        }
#endif
    } else {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            closesocket(fd);
            return RTCP_EIO;
        }
    }
    return fd;
}

static int udp_join(int fd, const uint8_t group[4])
{
    if (fd < 0 || group == NULL) return RTCP_EINVAL;
#ifdef IP_ADD_MEMBERSHIP
    struct ip_mreq request;
    memset(&request, 0, sizeof(request));
    memcpy(&request.imr_multiaddr, group, 4U);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request,
                   sizeof(request)) != 0) {
        return RTCP_EIO;
    }
    return RTCP_OK;
#else
    (void)group;
    return RTCP_ENOTSUP;
#endif
}

static int udp_sendto(int fd, const rtcp_addr_t *to, const void *buf, size_t len)
{
    if (fd < 0 || to == NULL || (buf == NULL && len != 0U)) return RTCP_EINVAL;
    struct sockaddr_storage storage;
    socklen_t length = 0;
    int rc = fill_sockaddr(to, &storage, &length);
    if (rc != RTCP_OK) return rc;
    rc = sendto(fd, buf, len, 0, (struct sockaddr *)&storage, length);
    return rc < 0 ? RTCP_EIO : rc;
}

static int udp_recvfrom(int fd, void *buf, size_t len, rtcp_addr_t *from)
{
    if (fd < 0 || (buf == NULL && len != 0U)) return RTCP_EINVAL;
    struct sockaddr_storage storage;
    socklen_t storage_len = sizeof(storage);
    int rc = recvfrom(fd, buf, len, 0, (struct sockaddr *)&storage, &storage_len);
    if (rc < 0) return RTCP_EIO;
    if (from != NULL) read_sockaddr(&storage, from);
    return rc;
}

uint16_t rtcp_net_rtthread_local_port(int fd)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&addr, &len) != 0) return 0U;
#if RTCP_NET_HAVE_IPV6
    if (addr.ss_family == AF_INET6) {
        return ntohs(((const struct sockaddr_in6 *)&addr)->sin6_port);
    }
#endif
    if (addr.ss_family == AF_INET) {
        return ntohs(((const struct sockaddr_in *)&addr)->sin_port);
    }
    return 0U;
}

const struct rtcp_net rtcp_net_rtthread = {
    .listen = listen_socket,
    .accept = accept_socket,
    .connect = connect_socket,
    .connect_timeout = connect_timeout_socket,
    .send = send_bytes,
    .recv = recv_bytes,
    .local_port = rtcp_net_rtthread_local_port,
    .poll = poll_fds,
    .close = close_fd,
    .udp_open = udp_open,
    .udp_join = udp_join,
    .udp_sendto = udp_sendto,
    .udp_recvfrom = udp_recvfrom,
};
