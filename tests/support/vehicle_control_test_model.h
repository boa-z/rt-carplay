/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "rtcp/vehicle.h"
#include "rtcp/opack.h"
#include "rtcp/rtcp.h"
#include <assert.h>
#include <string.h>
#include "vehicle_control_fixtures.h"

/* Synthetic command IDs/types, not a real vehicle control/actuator mapping. */
static unsigned control_prepares, control_commits;
static int64_t control_last_iid;
static bool control_bad_response;
static bool control_prepare(void *ctx, int64_t iid, const rtcp_vehicle_control_values_t *args,
                            rtcp_vehicle_control_values_t *out)
{
    assert(ctx == &control_commits);
    ++control_prepares;
    if (iid == 10)
    {
        assert(args->count == 1 && args->values[0].format == RTCP_VEHICLE_UINT8);
        if (args->values[0].as.integer == 95)
            return false;
        out->count = 1;
        out->values[0] = (rtcp_vehicle_value_t){ .format = RTCP_VEHICLE_UINT8,
                                                 .as.integer = control_bad_response ? 256 : args->values[0].as.integer + 1 };
    }
    else if (iid == 11)
        assert(!args->count);
    else
    {
        assert(iid == 12 && !args->count);
        out->count = 1;
        out->values[0].format = RTCP_VEHICLE_DATA;
        out->values[0].as.blob.length = 6;
        memcpy(out->values[0].as.blob.bytes, "\0\xff"
                                             "demo",
               6);
    }
    return true;
}
static void control_commit(void *ctx, int64_t iid, const rtcp_vehicle_control_values_t *args)
{
    assert(ctx == &control_commits);
    ++control_commits;
    control_last_iid = iid;
    if (iid == 10)
        assert(args->count == 1 && args->values[0].as.integer == 7);
    else
        assert(!args->count);
}
static rtcp_vehicle_t *vehicle_control_test_create(void)
{
    rtcp_vehicle_characteristic_t s = { .accessory_iid = 1, .service_iid = 2, .iid = 3, .accessory_type = 100, .service_type = 200, .type = 1003, .format = RTCP_VEHICLE_UINT32, .owner = RTCP_VEHICLE_SHARED, .mutable_value = true, .has_initial = true, .initial = { .format = RTCP_VEHICLE_UINT32, .as.integer = 7 } };
    char name[] = "level", echo[] = "echo", blob[] = "blob";
    rtcp_vehicle_control_parameter_t p = { .name = name, .format = RTCP_VEHICLE_UINT8 },
                                     q = { .name = echo, .format = RTCP_VEHICLE_UINT8 },
                                     b = { .name = blob, .format = RTCP_VEHICLE_DATA, .maximum_length = 8 };
    rtcp_vehicle_control_t c[] = {
        { .service_iid = 2, .iid = 10, .type = 1010, .urgent = true, .request = &p, .request_count = 1, .response = &q, .response_count = 1 },
        { .service_iid = 2, .iid = 11, .type = 1011 },
        { .service_iid = 2, .iid = 12, .type = 1012, .response = &b, .response_count = 1 }
    };
    rtcp_vehicle_config_t config = { .struct_size = sizeof(config), .plugin_name = "test", .characteristics = &s, .count = 1, .context = &control_commits, .controls = c, .control_count = 3, .prepare_control = control_prepare, .did_control = control_commit };
    rtcp_vehicle_t *v = NULL;
    assert(rtcp_vehicle_create(&config, &v) == RTCP_OK);
    memset(name, 'x', 5);
    memset(echo, 'x', 4);
    memset(blob, 'x', 4);
    memset(c, 0, sizeof(c));
    return v;
}
static void vehicle_control_test_reply(const uint8_t *body, size_t length)
{
    rtcp_opack_node_t n[128];
    size_t count;
    assert(rtcp_opack_decode(body, length, n, 128, &count) == RTCP_OPACK_OK);
    size_t at = rtcp_opack_dict_get(n, count, 0, "pluginData");
    assert(at != SIZE_MAX);
    const uint8_t *data = n[at].value.blob.bytes;
    size_t bytes = n[at].value.blob.length;
    assert(rtcp_opack_decode(data, bytes, n, 128, &count) == RTCP_OPACK_OK);
    at = rtcp_opack_dict_get(n, count, 0, "values");
    assert(at != SIZE_MAX && n[at].count == 2);
    assert(n[at + 1].value.integer == 10);
    size_t echo = rtcp_opack_dict_get(n, count, at + 2, "echo");
    assert(echo != SIZE_MAX && n[echo].value.integer == 8);
    size_t errors = rtcp_opack_dict_get(n, count, 0, "errors");
    assert(errors != SIZE_MAX && n[errors].count == 1);
    assert(n[errors + 1].value.integer == 99 && n[errors + 2].value.integer == -6735);
}
