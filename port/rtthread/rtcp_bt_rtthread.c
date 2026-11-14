/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * RT-Thread BT seam.
 *
 * The target path uses the Zephyr Bluetooth host (Classic BR/EDR + RFCOMM)
 * that ships with Luban-Lite.  Wiring it up requires board validation of the
 * RTL8733BS H5 transport, so this file only provides the seam and explicit
 * errors; it never pretends a link exists.  The RFCOMM data path can be
 * implemented over bt_rfcomm_dlc without touching the CarPlay core.
 */
#include "rtcp_rtthread.h"

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

const struct rtcp_bt rtcp_bt_rtthread = {
    .enable = bt_enable,
    .rfcomm_listen = bt_rfcomm_listen,
    .rfcomm_accept = bt_rfcomm_accept,
    .send = bt_send,
    .recv = bt_recv,
    .set_event_cb = bt_set_event_cb,
};
