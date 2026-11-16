/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vehicle_control_test_model.h"
#include "airplay_info_plist.h"
#include "receiver_vehicle.h"
#include <stdio.h>
static uint8_t reply[16384];
static bool dump;
static void hex(const char *name, const uint8_t *bytes, size_t count)
{
    if (!dump)
        return;
    printf("CONTROL %s ", name);
    for (size_t i = 0; i < count; ++i) printf("%02x", bytes[i]);
    puts("");
}
static void catalogue_validation(void)
{
    rtcp_vehicle_characteristic_t s = { .accessory_iid = 1, .service_iid = 2, .iid = 3, .format = RTCP_VEHICLE_BOOL };
    rtcp_vehicle_control_parameter_t p[] = { { .name = "one", .format = RTCP_VEHICLE_DATA, .maximum_length = 8 },
                                             { .name = "two", .format = RTCP_VEHICLE_UINT8 } };
    rtcp_vehicle_control_t c[] = { { .service_iid = 2, .iid = 10, .type = 1010, .request = p, .request_count = 2 },
                                   { .service_iid = 2, .iid = 11, .type = 1011 } };
    rtcp_vehicle_config_t cfg = { .struct_size = sizeof(cfg), .plugin_name = "validation", .characteristics = &s, .count = 1, .controls = c, .control_count = 2, .prepare_control = control_prepare };
    rtcp_vehicle_t *v;
    assert(rtcp_vehicle_create(&cfg, &v) == RTCP_OK);
    rtcp_vehicle_destroy(v);
    c[1].iid = 10;
    assert(rtcp_vehicle_create(&cfg, &v) == RTCP_EINVAL);
    c[1].iid = 11;
    c[0].iid = 3;
    assert(rtcp_vehicle_create(&cfg, &v) == RTCP_EINVAL);
    c[0].iid = 10;
    c[0].service_iid = 99;
    assert(rtcp_vehicle_create(&cfg, &v) == RTCP_EINVAL);
    c[0].service_iid = 2;
    c[0].request_count = 5;
    assert(rtcp_vehicle_create(&cfg, &v) == RTCP_EINVAL);
    c[0].request_count = 2;
    p[0].maximum_length = 257;
    assert(rtcp_vehicle_create(&cfg, &v) == RTCP_EINVAL);
    p[0].maximum_length = 8;
    p[1].name = "one";
    assert(rtcp_vehicle_create(&cfg, &v) == RTCP_EINVAL);
    p[1].name = "two";
    p[1].format = (rtcp_vehicle_format_t)99;
    assert(rtcp_vehicle_create(&cfg, &v) == RTCP_EINVAL);
    p[1].format = RTCP_VEHICLE_UINT8;
    cfg.prepare_control = NULL;
    assert(rtcp_vehicle_create(&cfg, &v) == RTCP_EINVAL);
}
static bool hold_delivery;
static unsigned delivered_controls;
static int deliver_control(void *ctx, rtcp_data_stream_kind_t kind, const uint8_t *body, size_t bytes)
{
    (void)ctx;
    assert(kind == RTCP_DATA_STREAM_VEHICLE);
    if (hold_delivery)
        return RTCP_ENOMEM;
    vehicle_control_test_reply(body, bytes);
    ++delivered_controls;
    return RTCP_OK;
}
static void retained_control_delivery(void)
{
    rtcp_vehicle_t *v = vehicle_control_test_create();
    receiver_vehicle_t *delivery = receiver_vehicle_create(v, deliver_control, NULL);
    assert(delivery);
    unsigned before = control_commits;
    hold_delivery = true;
    assert(receiver_vehicle_body(delivery, RTCP_DATA_STREAM_VEHICLE, control_mixed, sizeof(control_mixed)));
    assert(control_commits == before + 2);
    for (unsigned i = 0; i < 3; ++i) receiver_vehicle_tick(delivery);
    assert(control_commits == before + 2 && !delivered_controls);
    assert(!receiver_vehicle_body(delivery, RTCP_DATA_STREAM_VEHICLE, control_mixed, sizeof(control_mixed)));
    assert(control_commits == before + 2);
    rtcp_vehicle_delivery_status_t status;
    receiver_vehicle_status(delivery, &status);
    assert(status.pending_reply_bytes[0] && status.queue_retries && status.rejected_requests == 1);
    hold_delivery = false;
    receiver_vehicle_tick(delivery);
    assert(delivered_controls == 1 && control_commits == before + 2);
    receiver_vehicle_status(delivery, &status);
    assert(!status.pending_reply_bytes[0]);
    /* A new valid request can commit again: no inferred exactly-once policy. */
    hold_delivery = true;
    assert(receiver_vehicle_body(delivery, RTCP_DATA_STREAM_VEHICLE, control_mixed, sizeof(control_mixed)));
    assert(control_commits == before + 4);
    receiver_vehicle_closed(delivery, RTCP_DATA_STREAM_VEHICLE, RTCP_EIO);
    receiver_vehicle_status(delivery, &status);
    assert(!status.pending_reply_bytes[0] && status.abandoned_replies == 1);
    assert(delivered_controls == 1 && control_commits == before + 4);
    receiver_vehicle_destroy(delivery);
    rtcp_vehicle_destroy(v);
    control_commits = control_prepares = 0;
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--check-info"))
    {
        FILE *f = fopen(argv[2], "rb");
        assert(f);
        size_t bytes = fread(reply, 1, sizeof(reply), f);
        assert(!ferror(f) && feof(f));
        fclose(f);
        rtcp_vehicle_t *v = vehicle_control_test_create();
        printf("%d\n", airplay_info_plist_matches_vehicle(reply, bytes, v));
        rtcp_vehicle_destroy(v);
        return 0;
    }
    dump = argc == 2 && !strcmp(argv[1], "--dump");
    catalogue_validation();
    retained_control_delivery();
    rtcp_vehicle_t *v = vehicle_control_test_create();
    size_t length;
    const struct
    {
        const uint8_t *bytes;
        size_t length;
    } invalid[] = {
        { control_wrong_type, sizeof(control_wrong_type) },
        { control_overflow, sizeof(control_overflow) },
        { control_unknown_parameter, sizeof(control_unknown_parameter) },
        { control_missing_parameter, sizeof(control_missing_parameter) },
        { control_late_invalid, sizeof(control_late_invalid) }
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        unsigned before = control_prepares;
        length = 999;
        assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, invalid[i].bytes, invalid[i].length, reply, sizeof(reply), &length) == RTCP_EINVAL);
        assert(!length && !control_commits && control_prepares == before);
    }
    for (size_t n = 1; n < sizeof(control_mixed); ++n)
    {
        assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, control_mixed, n, reply, sizeof(reply), &length) == RTCP_EINVAL);
        assert(!control_commits);
    }
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, control_refused, sizeof(control_refused), reply, sizeof(reply), &length) == RTCP_ESTATE);
    assert(!control_commits);
    control_bad_response = true;
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, control_mixed, sizeof(control_mixed), reply, sizeof(reply), &length) == RTCP_EINVAL);
    assert(!control_commits);
    control_bad_response = false;
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, control_mixed, sizeof(control_mixed), reply, 1, &length) == RTCP_ENOMEM);
    assert(!length && !control_commits);
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, control_mixed, sizeof(control_mixed), reply, sizeof(reply), &length) == RTCP_OK);
    assert(control_commits == 2 && control_last_iid == 11);
    vehicle_control_test_reply(reply, length);
    hex("mixed", reply, length);
    /* Every too-small output capacity must preserve the post-commit count. */
    size_t needed = length;
    for (size_t cap = 0; cap < needed; ++cap)
    {
        assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, control_mixed, sizeof(control_mixed), reply, cap, &length) == RTCP_ENOMEM);
        assert(!length && control_commits == 2);
    }
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_HIGH, control_blob, sizeof(control_blob), reply, sizeof(reply), &length) == RTCP_OK);
    assert(control_commits == 3);
    hex("blob", reply, length);
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_HIGH, control_empty, sizeof(control_empty), reply, sizeof(reply), &length) == RTCP_OK);
    assert(control_commits == 4);
    hex("empty", reply, length);
    rtcp_vehicle_status_t status;
    assert(rtcp_vehicle_status(v, &status) == RTCP_OK && status.controls == 3 && status.committed_controls == 4);
    assert(status.last_method == RTCP_VEHICLE_METHOD_CONTROL && status.last_lane == RTCP_VEHICLE_HIGH && status.last_result == RTCP_OK);
    assert(status.method_responses[RTCP_VEHICLE_METHOD_CONTROL] == 3);
    rtcp_vehicle_reset_session(v);
    assert(rtcp_vehicle_status(v, &status) == RTCP_OK && status.controls == 3 && status.committed_controls == 4);
    airplay_info_plist_config_t info = { .device_id = "AA:BB:CC:DD:EE:FF", .width_pixels = 1024, .height_pixels = 600, .vehicle = v };
    assert(airplay_info_plist_build(&info, reply, sizeof(reply), &length));
    assert(airplay_info_plist_matches_vehicle(reply, length, v));
    if (dump)
    {
        printf("CONTROLINFO ");
        for (size_t i = 0; i < length; ++i) printf("%02x", reply[i]);
        puts("");
    }
    rtcp_vehicle_destroy(v);
    puts("Vehicle controls: copied catalogue/arguments, bounded schemas, pure prepare, commit after encoding and independent fixtures PASS");
    return 0;
}
