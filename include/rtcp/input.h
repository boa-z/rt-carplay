/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_INPUT_H
#define RTCP_INPUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

struct rtcp_receiver;
/* Owner-thread output only, including callbacks on that owner. The host
 * retains touch ownership, coordinate conversion and HID descriptor policy.
 * Report bytes are consumed before return. False means no usable encrypted
 * event channel (or a failed write); these calls never create a session. */
bool rtcp_receiver_send_hid_report(struct rtcp_receiver *, uint32_t hid_uid,
                                   const uint8_t *report, size_t length);
bool rtcp_receiver_request_siri(struct rtcp_receiver *);

struct rtcp_touch
{
    uint16_t x;
    uint16_t y;
    uint8_t id;
    uint8_t action; /* RTCP_TOUCH_DOWN / MOVE / UP */
};

#define RTCP_TOUCH_DOWN 1
#define RTCP_TOUCH_MOVE 2
#define RTCP_TOUCH_UP   3

struct rtcp_input
{
    void (*set_cb)(void (*cb)(const struct rtcp_touch *ev, void *user),
                   void *user);
};

#ifdef __cplusplus
}
#endif
#endif /* RTCP_INPUT_H */
