/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file net.h
 * Socket provider and portable address representation. */
#ifndef RTCP_NET_H
#define RTCP_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** IP address, port in host byte order, and optional IPv6 zone. */
typedef struct rtcp_addr
{
    uint8_t family; /* RTCP_AF_INET / RTCP_AF_INET6 */
    uint8_t addr[16];
    uint16_t port;
    /* IPv6 zone (netif index) for link-local addresses; sockets need it to
     * pick the outgoing interface.  Ignored for IPv4. */
    uint16_t scope_id;
} rtcp_addr_t;

#define RTCP_AF_INET  4
#define RTCP_AF_INET6 6

#define RTCP_POLLIN  0x01
#define RTCP_POLLOUT 0x02
#define RTCP_POLLERR 0x04

/* RTSP, iAP, event, timing, Main/alternate screen, eight audio data/control pairs and three
 * optional duplex Type 130 endpoints can be ready together. */
#ifndef RTCP_NET_POLL_MAX
#define RTCP_NET_POLL_MAX 25
#endif

/** Poll descriptor with requested events and returned readiness bits. */
struct rtcp_pollfd
{
    int fd;
    int events;
    int revents;
};

/** Platform socket operations on the owner thread. Open operations return
 * a descriptor or negative RTCP error. Send/receive return byte counts
 * or negative errors. Close releases the descriptor. The Receiver borrows
 * this table while alive. */
struct rtcp_net
{
    /** TCP listener bound to INADDR_ANY (RTCP_AF_INET) or in6addr_any
     * (RTCP_AF_INET6).  RTCP_ENOTSUP when the port has no IPv6 support. */
    int (*listen)(uint8_t family, uint16_t port);
    /** Accept a TCP peer and write its address on success. The caller
     * owns the returned descriptor and closes it with this provider. */
    int (*accept)(int lfd, rtcp_addr_t *peer);
    /** Open a connected TCP descriptor or return a negative RTCP error. */
    int (*connect)(const rtcp_addr_t *addr);
    /** Bounded TCP connect: returns a connected fd, or a negative RTCP error
     * (RTCP_ETIMEDOUT when the handshake did not finish in time).  Optional;
     * callers fall back to the blocking connect() when NULL. */
    int (*connect_timeout)(const rtcp_addr_t *addr, int timeout_ms);
    /** Consume bytes synchronously. A nonnegative count may be short. */
    int (*send)(int fd, const void *buf, size_t len);
    /** Write into caller storage. Zero means TCP EOF. */
    int (*recv)(int fd, void *buf, size_t len);
    /** Local port of a bound TCP or UDP socket; 0 when unknown. */
    uint16_t (*local_port)(int fd);
    /** Fill revents for the borrowed array and return its ready count,
     * zero on timeout, or a negative RTCP error. Zero timeout never blocks. */
    int (*poll)(struct rtcp_pollfd *fds, int n, int timeout_ms);
    void (*close)(int fd);

    /** UDP socket bound to INADDR_ANY:port (RTCP_AF_INET) or in6addr_any:port
     * (RTCP_AF_INET6).  reuse enables SO_REUSEADDR/SO_REUSEPORT where
     * available.  udp_join adds an IPv4 socket to a multicast group (the mDNS
     * responder); udp_sendto/udp_recvfrom handle both families. */
    int (*udp_open)(uint8_t family, uint16_t port, bool reuse);
    int (*udp_join)(int fd, const uint8_t group[4]);
    int (*udp_sendto)(int fd, const rtcp_addr_t *to, const void *buf, size_t len);
    int (*udp_recvfrom)(int fd, void *buf, size_t len, rtcp_addr_t *from);
};

#endif /* RTCP_NET_H */
