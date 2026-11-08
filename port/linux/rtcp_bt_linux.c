/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Linux BT seam stub.  A desktop Linux host has no Classic BR/EDR accessory
 * stack equivalent to the target's Zephyr/Realtek integration, so the
 * simulator cannot accept an RFCOMM link.  Return a clear error instead of
 * pretending a connection exists; the simulator drives iAP2 over an injected
 * transport (see examples/linux_sim).
 */
#include "rtcp_linux.h"

#include "rtcp/rtcp.h"

static int bt_enable(const char *name, const uint8_t bdaddr[6])
{
    (void)name;
    (void)bdaddr;
    return RTCP_ENOTSUP;
}

static int bt_rfcomm_listen(uint8_t channel, const char *name,
                            const char *uuid128, uint8_t *out_channel)
{
    (void)channel;
    (void)name;
    (void)uuid128;
    (void)out_channel;
    return RTCP_ENOTSUP;
}

static int bt_rfcomm_accept(int lfd, int *fd)
{
    (void)lfd;
    (void)fd;
    return RTCP_ENOTSUP;
}

static int bt_send(int fd, const void *buf, size_t len)
{
    (void)fd;
    (void)buf;
    (void)len;
    return RTCP_ENOTSUP;
}

static int bt_recv(int fd, void *buf, size_t len)
{
    (void)fd;
    (void)buf;
    (void)len;
    return RTCP_ENOTSUP;
}

static void bt_set_event_cb(void (*cb)(int ev, void *user), void *user)
{
    (void)cb;
    (void)user;
}

const struct rtcp_bt rtcp_bt_linux = {
    .enable = bt_enable,
    .rfcomm_listen = bt_rfcomm_listen,
    .rfcomm_accept = bt_rfcomm_accept,
    .send = bt_send,
    .recv = bt_recv,
    .set_event_cb = bt_set_event_cb,
};
