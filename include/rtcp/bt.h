/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_BT_H
#define RTCP_BT_H

#include <stddef.h>
#include <stdint.h>

#define RTCP_BT_EV_CONNECTED    1
#define RTCP_BT_EV_DISCONNECTED 2
#define RTCP_BT_EV_LINK_KEY     3

struct rtcp_bt {
    int (*enable)(const char *name, const uint8_t bdaddr[6]);
    int (*rfcomm_listen)(uint8_t channel, const char *name,
                         const char *uuid128, uint8_t *out_channel);
    int (*rfcomm_accept)(int lfd, int *fd);
    int (*send)(int fd, const void *buf, size_t len);
    int (*recv)(int fd, void *buf, size_t len);
    void (*set_event_cb)(void (*cb)(int ev, void *user), void *user);
};

#endif /* RTCP_BT_H */
