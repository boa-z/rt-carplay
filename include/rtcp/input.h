/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file input.h
 * HID and Siri output contracts for Classic CarPlay. */
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
/** Owner-thread output only, including callbacks on that owner. The host
 * retains touch ownership, coordinate conversion and HID descriptor policy.
 * Report bytes are consumed before return. False means no usable encrypted
 * event channel (or a failed write); these calls never create a session. */
bool rtcp_receiver_send_hid_report(struct rtcp_receiver *, uint32_t hid_uid,
                                   const uint8_t *report, size_t length);
/** Request Siri on the current encrypted event channel from its owner.
 * Returns true when written, false when unavailable or the write fails. */
bool rtcp_receiver_request_siri(struct rtcp_receiver *);

/** One slot in the negotiated two-contact touch report. */
typedef struct
{
    uint16_t x, y;
    bool down;
} rtcp_touch_contact_t;

#define RTCP_TOUCH_CONTACTS 2U

/** Send a complete report ordered by contact slot. An inactive slot is up.
 * Use negotiated logical pixels. Returns false if the event write fails. */
bool rtcp_receiver_send_touch(struct rtcp_receiver *, const rtcp_touch_contact_t contacts[RTCP_TOUCH_CONTACTS]);

/** Joystick/rotary state for the matching HID descriptor. */
typedef struct
{
    bool select, home, back;
    int8_t x, y, wheel;
} rtcp_knob_report_t;

/** Consumer Control actions. Release clears the previous key state. */
typedef enum
{
    RTCP_MEDIA_RELEASE = 0,
    RTCP_MEDIA_PLAY,
    RTCP_MEDIA_PAUSE,
    RTCP_MEDIA_PLAY_PAUSE,
    RTCP_MEDIA_NEXT,
    RTCP_MEDIA_PREVIOUS,
} rtcp_media_button_t;

/** Advertise the matching HID devices in /info before sending a knob report.
 * Send neutral state on release. The host owns physical button mapping and
 * local volume keys. Axes and wheel accept -127 through 127. Returns
 * false for invalid input or a failed event write. */
bool rtcp_receiver_send_knob(struct rtcp_receiver *, const rtcp_knob_report_t *);
/** Send a media key on the owner, followed by a separate release report
 * when the key goes up. Returns false for invalid input or write failure. */
bool rtcp_receiver_send_media_button(struct rtcp_receiver *, rtcp_media_button_t);

/** Platform pointer event before conversion to negotiated display pixels. */
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

/** Platform event source. The port owns callback scheduling and event
 * storage. Consumers copy a delivered event before returning. */
struct rtcp_input
{
    void (*set_cb)(void (*cb)(const struct rtcp_touch *ev, void *user),
                   void *user);
};

#ifdef __cplusplus
}
#endif
#endif /* RTCP_INPUT_H */
