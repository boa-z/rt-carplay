/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_INPUT_H
#define RTCP_INPUT_H

#include <stddef.h>
#include <stdint.h>

struct rtcp_touch {
    uint16_t x;
    uint16_t y;
    uint8_t  id;
    uint8_t  action;   /* RTCP_TOUCH_DOWN / MOVE / UP */
};

#define RTCP_TOUCH_DOWN 1
#define RTCP_TOUCH_MOVE 2
#define RTCP_TOUCH_UP   3

struct rtcp_input {
    void (*set_cb)(void (*cb)(const struct rtcp_touch *ev, void *user),
                   void *user);
};

#endif /* RTCP_INPUT_H */
