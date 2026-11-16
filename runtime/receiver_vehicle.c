/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "receiver_vehicle.h"
#include "rtcp/rtcp.h"
#include <stdlib.h>
#include <string.h>

/* One retained reply per lane plus one coalesced notification. No embedded
 * stack buffers: the workspace exists only for explicitly attached vehicles. */
#define WIRE_BYTES 16384U
typedef struct
{
    size_t length;
    uint8_t bytes[WIRE_BYTES];
} pending_t;
struct receiver_vehicle
{
    rtcp_vehicle_t *vehicle;
    receiver_vehicle_send_t send;
    void *context;
    pending_t reply[2], notification;
    rtcp_vehicle_delivery_status_t status;
};

receiver_vehicle_t *receiver_vehicle_create(rtcp_vehicle_t *v,
                                            receiver_vehicle_send_t send, void *context)
{
    if (!v || !send)
        return NULL;
    receiver_vehicle_t *s = calloc(1, sizeof(*s));
    if (s)
    {
        s->vehicle = v;
        s->send = send;
        s->context = context;
    }
    return s;
}
void receiver_vehicle_destroy(receiver_vehicle_t *s)
{
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    free(s);
}
void receiver_vehicle_reset(receiver_vehicle_t *s)
{
    if (!s)
        return;
    for (unsigned i = 0; i < 2; ++i)
    {
        if (s->reply[i].length)
            ++s->status.abandoned_replies;
        s->reply[i].length = 0;
    }
    if (s->notification.length)
        ++s->status.abandoned_notifications;
    s->notification.length = 0;
    rtcp_vehicle_reset_session(s->vehicle);
}
static int flush(receiver_vehicle_t *s, pending_t *p, rtcp_data_stream_kind_t kind)
{
    if (!p->length)
        return RTCP_OK;
    int rc = s->send(s->context, kind, p->bytes, p->length);
    if (!rc)
    {
        p->length = 0;
        ++s->status.queued_messages;
    }
    else
    {
        s->status.last_error = rc;
        if (rc == RTCP_ENOMEM)
            ++s->status.queue_retries;
    }
    return rc;
}
bool receiver_vehicle_body(receiver_vehicle_t *s, rtcp_data_stream_kind_t kind,
                           const uint8_t *body, size_t length)
{
    if (!s || (kind != RTCP_DATA_STREAM_VEHICLE && kind != RTCP_DATA_STREAM_VEHICLE_HIGH))
        return false;
    unsigned lane = kind == RTCP_DATA_STREAM_VEHICLE ? 0 : 1;
    pending_t *p = &s->reply[lane];
    /* Retry the previous reply before accepting another side-effecting request.
     * Exhaustion closes this lane explicitly; the new request is not committed. */
    if (flush(s, p, kind) != RTCP_OK)
    {
        ++s->status.rejected_requests;
        return false;
    }
    int rc = rtcp_vehicle_handle(s->vehicle, lane ? RTCP_VEHICLE_HIGH : RTCP_VEHICLE_NORMAL,
                                 body, length, p->bytes, sizeof(p->bytes), &p->length);
    if (rc != RTCP_OK)
    {
        s->status.last_error = rc;
        ++s->status.rejected_requests;
        return false;
    }
    rc = flush(s, p, kind);
    return rc == RTCP_OK || rc == RTCP_ENOMEM;
}
void receiver_vehicle_closed(receiver_vehicle_t *s, rtcp_data_stream_kind_t kind, int error)
{
    if (!s || (kind != RTCP_DATA_STREAM_VEHICLE && kind != RTCP_DATA_STREAM_VEHICLE_HIGH))
        return;
    pending_t *p = &s->reply[kind == RTCP_DATA_STREAM_VEHICLE ? 0 : 1];
    if (p->length)
    {
        ++s->status.abandoned_replies;
        p->length = 0;
    }
    s->status.last_error = error;
    /* Subscriptions and an unsent HIGH notification survive selective channel
     * retirement within the same authenticated session. Full reset clears them. */
}
void receiver_vehicle_tick(receiver_vehicle_t *s)
{
    if (!s)
        return;
    (void)flush(s, &s->reply[0], RTCP_DATA_STREAM_VEHICLE);
    if (flush(s, &s->reply[1], RTCP_DATA_STREAM_VEHICLE_HIGH) != RTCP_OK)
        return;
    pending_t *p = &s->notification;
    if (p->length && flush(s, p, RTCP_DATA_STREAM_VEHICLE_HIGH) != RTCP_OK)
        return;
    rtcp_vehicle_lane_t lane;
    int rc = rtcp_vehicle_notification(s->vehicle, &lane, p->bytes, sizeof(p->bytes), &p->length);
    if (rc == RTCP_OK)
        (void)flush(s, p, RTCP_DATA_STREAM_VEHICLE_HIGH);
    else if (rc != RTCP_ENOENT)
        s->status.last_error = rc;
}
void receiver_vehicle_status(const receiver_vehicle_t *s, rtcp_vehicle_delivery_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s)
        return;
    *out = s->status;
    out->attached = true;
    out->pending_reply_bytes[0] = s->reply[0].length;
    out->pending_reply_bytes[1] = s->reply[1].length;
    out->pending_notification_bytes = s->notification.length;
}
