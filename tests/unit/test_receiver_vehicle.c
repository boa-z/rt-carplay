/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "receiver_vehicle.h"
#include "vehicle_test_wire.h"
#include <stdio.h>

static int send_result;
static uint8_t captured[16384]; static size_t captured_length;
static rtcp_data_stream_kind_t captured_kind;
static int64_t next_notification;
static unsigned checked_notifications;
static int send_body(void *ctx, rtcp_data_stream_kind_t kind, const uint8_t *bytes, size_t length)
{
    assert(ctx == &send_result);
    if (send_result) return send_result;
    if (next_notification) {
        assert(kind == RTCP_DATA_STREAM_VEHICLE_HIGH);
        vehicle_test_reply(bytes, length, "updateNotify", false, next_notification++);
        ++checked_notifications;
    }
    assert(length <= sizeof(captured)); memcpy(captured, bytes, length);
    captured_length = length; captured_kind = kind; return RTCP_OK;
}
static rtcp_vehicle_delivery_status_t delivery(receiver_vehicle_t *s)
{ rtcp_vehicle_delivery_status_t status; receiver_vehicle_status(s, &status); return status; }
int main(void)
{
    rtcp_vehicle_t *v = vehicle_test_create();
    receiver_vehicle_t *s = receiver_vehicle_create(v, send_body, &send_result); assert(s);
    uint8_t request[512]; size_t n = vehicle_test_request("registerRequest", false, 0, request, sizeof(request));
    send_result = RTCP_ENOMEM;
    assert(receiver_vehicle_body(s, RTCP_DATA_STREAM_VEHICLE, request, n));
    assert(delivery(s).pending_reply_bytes[0]);
    rtcp_vehicle_status_t vs; assert(rtcp_vehicle_status(v, &vs) == RTCP_OK && vs.subscribed == 1);
    receiver_vehicle_tick(s); assert(delivery(s).pending_reply_bytes[0]);
    send_result = 0; receiver_vehicle_tick(s);
    assert(!delivery(s).pending_reply_bytes[0] && captured_kind == RTCP_DATA_STREAM_VEHICLE);
    vehicle_test_reply(captured, captured_length, "registerResponse", true, 7);
    rtcp_vehicle_value_t value = {.format = RTCP_VEHICLE_UINT32, .as.integer = 42};
    assert(rtcp_vehicle_set(v, 3, &value) == RTCP_OK);
    send_result = RTCP_ENOMEM; receiver_vehicle_tick(s);
    assert(delivery(s).pending_notification_bytes);
    size_t pending = delivery(s).pending_notification_bytes;
    value.as.integer = 43; assert(rtcp_vehicle_set(v, 3, &value) == RTCP_OK);
    receiver_vehicle_tick(s); assert(delivery(s).pending_notification_bytes == pending);
    receiver_vehicle_closed(s, RTCP_DATA_STREAM_VEHICLE_HIGH, RTCP_EIO);
    send_result = RTCP_ESTATE; receiver_vehicle_tick(s);
    assert(delivery(s).pending_notification_bytes == pending);
    next_notification = 42; send_result = 0; receiver_vehicle_tick(s); next_notification = 0;
    /* The retained 42 is queued before the newer coalesced value 43. */
    assert(captured_kind == RTCP_DATA_STREAM_VEHICLE_HIGH);
    vehicle_test_reply(captured, captured_length, "updateNotify", false, 43);
    assert(checked_notifications == 2);
    assert(!delivery(s).pending_notification_bytes);

    n = vehicle_test_request("writeRequest", true, 50, request, sizeof(request));
    send_result = RTCP_ENOMEM;
    assert(receiver_vehicle_body(s, RTCP_DATA_STREAM_VEHICLE_HIGH, request, n));
    n = vehicle_test_request("writeRequest", true, 60, request, sizeof(request));
    assert(!receiver_vehicle_body(s, RTCP_DATA_STREAM_VEHICLE_HIGH, request, n));
    bool present; assert(rtcp_vehicle_get(v, 3, &value, &present) == RTCP_OK && present && value.as.integer == 50);
    assert(delivery(s).rejected_requests == 1);
    receiver_vehicle_closed(s, RTCP_DATA_STREAM_VEHICLE_HIGH, RTCP_EIO);
    assert(delivery(s).abandoned_replies == 1);
    receiver_vehicle_tick(s); assert(delivery(s).pending_notification_bytes);
    receiver_vehicle_reset(s);
    assert(!delivery(s).pending_notification_bytes && delivery(s).abandoned_notifications == 1);
    assert(rtcp_vehicle_status(v, &vs) == RTCP_OK && !vs.subscribed && !vs.pending_notifications);
    assert(rtcp_vehicle_get(v, 3, &value, &present) == RTCP_OK && present && value.as.integer == 50);
    n = vehicle_test_request("readRequest", false, 0, request, sizeof(request));
    send_result = 0; assert(receiver_vehicle_body(s, RTCP_DATA_STREAM_VEHICLE, request, n));
    vehicle_test_reply(captured, captured_length, "readResponse", true, 50);
    request[0] = 0xff; assert(!receiver_vehicle_body(s, RTCP_DATA_STREAM_VEHICLE, request, n));
    assert(!receiver_vehicle_body(s, RTCP_DATA_STREAM_UI_SYNC, request, n));
    receiver_vehicle_destroy(s); rtcp_vehicle_destroy(v);
    puts("receiver vehicle: retained replies/notifications, bounded rejection, selective close and session reset passed");
    return 0;
}
