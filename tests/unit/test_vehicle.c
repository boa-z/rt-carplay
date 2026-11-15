/* SPDX-License-Identifier: GPL-3.0-or-later
 * Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI).
 * The configRequest wire fixture is from vehicle_state/tests.rs.
 */
#include "rtcp/vehicle.h"
#include "rtcp/opack.h"
#include "rtcp/rtcp.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static rtcp_opack_node_t nodes[512], opened[512];
static uint8_t request[16384], response[16384], inner[16384];
static size_t used, opened_count;
static unsigned commits;
static int64_t committed_iid;
static bool policy(void *ctx, int64_t iid, const rtcp_vehicle_value_t *value)
{
    assert(ctx == &commits);
    return iid != 4 || value->as.integer != 95;
}
static void committed(void *ctx, int64_t iid, const rtcp_vehicle_value_t *value)
{
    assert(ctx == &commits && value);
    ++commits; committed_iid = iid;
}
static rtcp_vehicle_value_t number(rtcp_vehicle_format_t format, int64_t n)
{
    rtcp_vehicle_value_t value = {.format = format}; value.as.integer = n; return value;
}
static rtcp_vehicle_characteristic_t spec(int64_t iid, rtcp_vehicle_format_t format,
                                          rtcp_vehicle_owner_t owner)
{
    rtcp_vehicle_characteristic_t s = {
        .accessory_iid = 1, .accessory_type = 100, .service_iid = 2,
        .service_type = 200, .iid = iid, .type = 1000 + iid,
        .format = format, .owner = owner, .mutable_value = true
    };
    return s;
}
static rtcp_vehicle_t *create(void)
{
    rtcp_vehicle_characteristic_t chars[4] = {
        spec(3, RTCP_VEHICLE_UINT32, RTCP_VEHICLE_CAR),
        spec(4, RTCP_VEHICLE_UINT8, RTCP_VEHICLE_SHARED),
        spec(5, RTCP_VEHICLE_STRING, RTCP_VEHICLE_PHONE),
        spec(6, RTCP_VEHICLE_BOOL, RTCP_VEHICLE_SHARED)
    };
    chars[0].has_initial = true; chars[0].initial = number(RTCP_VEHICLE_UINT32, 7);
    chars[1].has_range = true; chars[1].minimum = 0; chars[1].maximum = 100; chars[1].step = 5;
    chars[1].has_initial = true; chars[1].initial = number(RTCP_VEHICLE_UINT8, 50);
    chars[2].maximum_length = 8;
    chars[3].mutable_value = false;
    chars[3].has_initial = true; chars[3].initial.format = RTCP_VEHICLE_BOOL;
    rtcp_vehicle_config_t config = {
        .struct_size = sizeof(config), .plugin_name = "test", .plugin_id = 0,
        .characteristics = chars, .count = 4, .validate_write = policy,
        .did_write = committed, .context = &commits
    };
    rtcp_vehicle_t *v = NULL;
    assert(rtcp_vehicle_create(&config, &v) == RTCP_OK && v);
    return v; /* Descriptors/initial data must have been copied. */
}
static size_t add(rtcp_opack_type_t type)
{
    assert(used < 512);
    size_t i = used++;
    nodes[i] = (rtcp_opack_node_t){.type = type, .end = (uint16_t)used};
    return i;
}
static void txt(const char *s)
{
    size_t i = add(RTCP_OPACK_STRING);
    nodes[i].value.blob.bytes = (const uint8_t *)s;
    nodes[i].value.blob.length = strlen(s);
}
static void num(int64_t n) { nodes[add(RTCP_OPACK_INT)].value.integer = n; }
static size_t message(const char *command, bool dict, const int64_t *ids,
                       const rtcp_opack_node_t *values, size_t count)
{
    used = 0; size_t root = add(RTCP_OPACK_DICT);
    txt("command"); txt(command); txt("transactionID"); num(INT64_C(-949082264887402311));
    txt("values"); size_t list = add(dict ? RTCP_OPACK_DICT : RTCP_OPACK_ARRAY);
    for (size_t i = 0; i < count; ++i) {
        num(ids[i]);
        if (dict) {
            size_t at = add(values[i].type);
            nodes[at].value = values[i].value;
        }
    }
    nodes[list].count = (uint16_t)count; nodes[list].end = (uint16_t)used;
    nodes[root].count = 3; nodes[root].end = (uint16_t)used;
    size_t length;
    assert(rtcp_opack_encode(nodes, used, inner, sizeof(inner), &length) == RTCP_OPACK_OK);
    used = 0; root = add(RTCP_OPACK_DICT);
    txt("pluginID"); num(0); txt("pluginData"); size_t data = add(RTCP_OPACK_DATA);
    nodes[data].value.blob.bytes = inner; nodes[data].value.blob.length = length;
    nodes[root].end = (uint16_t)used; nodes[root].count = 2;
    assert(rtcp_opack_encode(nodes, used, request, sizeof(request), &length) == RTCP_OPACK_OK);
    return length;
}
static size_t key(const char *s)
{
    size_t at = rtcp_opack_dict_get(opened, opened_count, 0, s);
    assert(at != SIZE_MAX); return at;
}
static void open_reply(size_t length, const char *command, bool transaction)
{
    assert(rtcp_opack_decode(response, length, opened, 512, &opened_count) == RTCP_OPACK_OK);
    assert(opened[key("pluginID")].value.integer == 0);
    size_t data = key("pluginData");
    const uint8_t *bytes = opened[data].value.blob.bytes; size_t len = opened[data].value.blob.length;
    assert(rtcp_opack_decode(bytes, len, opened, 512, &opened_count) == RTCP_OPACK_OK);
    size_t cmd = key("command");
    assert(opened[cmd].type == RTCP_OPACK_STRING);
    assert(opened[cmd].value.blob.length == strlen(command));
    assert(memcmp(opened[cmd].value.blob.bytes, command, strlen(command)) == 0);
    if (transaction) {
        assert(opened[key("transactionID")].type == RTCP_OPACK_INT);
        assert(opened[key("transactionID")].value.integer == INT64_C(-949082264887402311));
    }
}
static const rtcp_opack_node_t *numeric_entry(const char *field, int64_t iid)
{
    size_t dict = key(field), at = dict + 1;
    assert(opened[dict].type == RTCP_OPACK_DICT);
    for (unsigned i = 0; i < opened[dict].count; ++i) {
        size_t value = opened[at].end;
        if (opened[at].value.integer == iid) return &opened[value];
        at = opened[value].end;
    }
    return NULL;
}
static void assert_value(rtcp_vehicle_t *v, int64_t iid, int64_t expected)
{
    rtcp_vehicle_value_t value; bool present;
    assert(rtcp_vehicle_get(v, iid, &value, &present) == RTCP_OK && present);
    assert(value.as.integer == expected);
}
static rtcp_opack_node_t int_node(int64_t value)
{
    rtcp_opack_node_t n = {.type = RTCP_OPACK_INT}; n.value.integer = value; return n;
}

static void config_fixture(void)
{
    static const uint8_t fixture[] = {
        0xe2,0x48,0x70,0x6c,0x75,0x67,0x69,0x6e,0x49,0x44,0x08,
        0x4a,0x70,0x6c,0x75,0x67,0x69,0x6e,0x44,0x61,0x74,0x61,0x91,0x2e,
        0xe2,0x47,0x63,0x6f,0x6d,0x6d,0x61,0x6e,0x64,0x4d,0x63,0x6f,0x6e,
        0x66,0x69,0x67,0x52,0x65,0x71,0x75,0x65,0x73,0x74,0x4d,0x74,0x72,
        0x61,0x6e,0x73,0x61,0x63,0x74,0x69,0x6f,0x6e,0x49,0x44,0x33,
        0xb9,0x60,0xc3,0x62,0xb5,0x2e,0xd4,0xf2
    };
    rtcp_vehicle_t *v = create(); size_t length;
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, fixture, sizeof(fixture),
                               response, sizeof(response), &length) == RTCP_OK);
    open_reply(length, "configResponse", true);
    size_t values = key("values");
    size_t accessories = rtcp_opack_dict_get(opened, opened_count, values, "accessories");
    assert(accessories != SIZE_MAX && opened[accessories].count == 1);
    assert(rtcp_vehicle_info(v, response, sizeof(response), &length) == RTCP_OK);
    assert(rtcp_opack_decode(response, length, opened, 512, &opened_count) == RTCP_OPACK_OK);
    assert(opened[key("pluginCount")].value.integer == 1);
    assert(opened[key("pluginConfigs")].count == 1);
    size_t config = key("pluginConfigs") + 1;
    accessories = rtcp_opack_dict_get(opened, opened_count, config, "accessories");
    size_t services = rtcp_opack_dict_get(opened, opened_count, accessories + 1, "services");
    size_t chars = rtcp_opack_dict_get(opened, opened_count, services + 1, "characteristics");
    assert(opened[chars].count == 4);
    size_t first = chars + 1;
    assert(!opened[rtcp_opack_dict_get(opened, opened_count, first, "writable")].value.boolean);
    assert(opened[rtcp_opack_dict_get(opened, opened_count, first, "oemWritable")].value.boolean);
    rtcp_vehicle_destroy(v);
}
static void subscriptions_and_transactions(void)
{
    rtcp_vehicle_t *v = create(); size_t length, input_length;
    const int64_t ids[] = {3,4,5,99}; rtcp_vehicle_status_t status;
    input_length = message("registerRequest", false, ids, NULL, 4);
    length = 99;
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_HIGH, request, input_length,
                               response, 1, &length) == RTCP_ENOMEM && length == 0);
    assert(rtcp_vehicle_status(v, &status) == RTCP_OK && status.subscribed == 0);
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_HIGH, request, input_length,
                               response, sizeof(response), &length) == RTCP_OK);
    open_reply(length, "registerResponse", true);
    assert(numeric_entry("values", 3)->value.integer == 7);
    assert(numeric_entry("errors", 5)->value.integer == -6727);
    assert(numeric_entry("errors", 99)->value.integer == -6727);
    assert(rtcp_vehicle_status(v, &status) == RTCP_OK && status.subscribed == 3);
    rtcp_vehicle_value_t value = number(RTCP_VEHICLE_UINT32, 8);
    assert(rtcp_vehicle_set(v, 3, &value) == RTCP_OK);
    value.as.integer = 9; assert(rtcp_vehicle_set(v, 3, &value) == RTCP_OK);
    rtcp_vehicle_lane_t lane = RTCP_VEHICLE_NORMAL;
    assert(rtcp_vehicle_notification(v, &lane, response, 1, &length) == RTCP_ENOMEM);
    assert(rtcp_vehicle_status(v, &status) == RTCP_OK && status.pending_notifications == 1);
    assert(rtcp_vehicle_notification(v, &lane, response, sizeof(response), &length) == RTCP_OK);
    assert(lane == RTCP_VEHICLE_HIGH);
    open_reply(length, "updateNotify", false);
    assert(opened[key("values")].count == 1 && numeric_entry("values", 3)->value.integer == 9);
    assert(rtcp_vehicle_set(v, 3, &value) == RTCP_OK);
    assert(rtcp_vehicle_notification(v, &lane, response, sizeof(response), &length) == RTCP_ENOENT);
    value.as.integer = 10; assert(rtcp_vehicle_set(v, 3, &value) == RTCP_OK);
    rtcp_vehicle_reset_session(v);
    assert(rtcp_vehicle_status(v, &status) == RTCP_OK && status.subscribed == 0 && !status.pending_notifications);
    assert_value(v, 3, 10);
    input_length = message("readRequest", false, ids, NULL, 4);
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, request, input_length,
                               response, sizeof(response), &length) == RTCP_OK);
    open_reply(length, "readResponse", true);
    assert(numeric_entry("values", 3)->value.integer == 10);
    assert(numeric_entry("errors", 99)->value.integer == -6727);
    rtcp_vehicle_destroy(v);
}
static void writes_and_policy(void)
{
    rtcp_vehicle_t *v = create(); size_t length;
    const int64_t ids[] = {3,4,6,99};
    rtcp_opack_node_t vals[] = {int_node(88),int_node(75),int_node(1),int_node(1)};
    size_t input_length = message("writeRequest", true, ids, vals, 4);
    unsigned before = commits;
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, request, input_length,
                               response, 1, &length) == RTCP_ENOMEM && !length);
    assert_value(v, 4, 50); assert(commits == before);
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, request, input_length,
                               response, sizeof(response), &length) == RTCP_OK);
    open_reply(length, "writeResponse", true);
    assert(numeric_entry("errors", 3)->value.integer == -6727);
    assert(numeric_entry("errors", 4)->value.integer == 0);
    assert(numeric_entry("errors", 6)->value.integer == -6727);
    assert(numeric_entry("errors", 99)->value.integer == -6727);
    assert_value(v, 3, 7); assert_value(v, 4, 75);
    assert(commits == before + 1 && committed_iid == 4);
    const int64_t shared[] = {4};
    const int64_t refused[] = {76,101,-5,95,256};
    for (size_t i = 0; i < sizeof(refused)/sizeof(refused[0]); ++i) {
        rtcp_opack_node_t val = int_node(refused[i]);
        input_length = message("writeRequest", true, shared, &val, 1);
        assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, request, input_length,
                                   response, sizeof(response), &length) == RTCP_OK);
        open_reply(length, "writeResponse", true);
        assert(numeric_entry("errors", 4)->value.integer == -6727);
        assert_value(v, 4, 75);
    }
    const int64_t phone[] = {5};
    input_length = message("registerRequest", false, phone, NULL, 1);
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, request, input_length,
                               response, sizeof(response), &length) == RTCP_OK);
    rtcp_opack_node_t val = {.type = RTCP_OPACK_STRING};
    val.value.blob.bytes = (const uint8_t *)"route"; val.value.blob.length = 5;
    input_length = message("writeRequest", true, phone, &val, 1);
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_HIGH, request, input_length,
                               response, sizeof(response), &length) == RTCP_OK);
    rtcp_vehicle_value_t got; bool present;
    assert(rtcp_vehicle_get(v, 5, &got, &present) == RTCP_OK && present);
    memset(request, 0, input_length); /* Stored string must not borrow the input. */
    assert(got.as.blob.length == 5 && memcmp(got.as.blob.bytes, "route", 5) == 0);
    rtcp_vehicle_lane_t lane;
    assert(rtcp_vehicle_notification(v, &lane, response, sizeof(response), &length) == RTCP_OK);
    open_reply(length, "updateNotify", false);
    assert(numeric_entry("values", 5)->value.blob.length == 5);
    rtcp_vehicle_status_t status;
    assert(rtcp_vehicle_status(v, &status) == RTCP_OK);
    assert(status.committed_writes == 2 && status.refused_writes == 8);
    rtcp_vehicle_destroy(v);
}
static void malformed_and_limits(void)
{
    rtcp_vehicle_t *v = create(); size_t length;
    int64_t ids[] = {4,4}; rtcp_opack_node_t vals[] = {int_node(25),int_node(30)};
    size_t len = message("writeRequest", true, ids, vals, 2);
    unsigned before = commits;
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, request, len,
                               response, sizeof(response), &length) == RTCP_EINVAL);
    assert_value(v, 4, 50); assert(commits == before);
    len = message("madeUpRequest", false, ids, NULL, 0);
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, request, len,
                               response, sizeof(response), &length) == RTCP_ENOTSUP);
    len = message("writeRequest", true, ids, vals, 1);
    for (size_t i = 0; i < len; ++i) {
        length = 999;
        assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, request, i,
                                   response, sizeof(response), &length) != RTCP_OK && !length);
    }
    len = message("controlRequest", true, ids, vals, 1);
    assert(rtcp_vehicle_handle(v, RTCP_VEHICLE_NORMAL, request, len,
                               response, sizeof(response), &length) == RTCP_OK);
    open_reply(length, "controlResponse", true);
    assert(numeric_entry("errors", 4)->value.integer == -6735);
    /* Deterministic malformed input corpus through both envelope decoders. */
    uint32_t state = 12345;
    for (unsigned i = 0; i < 1000; ++i) {
        size_t size = i % 129;
        for (size_t j = 0; j < size; ++j) {
            state = state * 1664525U + 1013904223U; request[j] = (uint8_t)(state >> 24);
        }
        (void)rtcp_vehicle_handle(v, RTCP_VEHICLE_HIGH, request, size,
                                  response, sizeof(response), &length);
    }
    assert_value(v, 4, 50); assert(commits == before);
    rtcp_vehicle_value_t value = number(RTCP_VEHICLE_UINT8, 51);
    assert(rtcp_vehicle_set(v, 4, &value) == RTCP_EINVAL);
    value = number(RTCP_VEHICLE_UINT32, -1);
    assert(rtcp_vehicle_set(v, 3, &value) == RTCP_EINVAL);
    assert(rtcp_vehicle_set(v, 99, &value) == RTCP_ENOENT);
    value.format = RTCP_VEHICLE_STRING;
    assert(rtcp_vehicle_set(v, 5, &value) == RTCP_ESTATE);
    value.format = RTCP_VEHICLE_BOOL;
    assert(rtcp_vehicle_set(v, 6, &value) == RTCP_ESTATE);
    rtcp_vehicle_destroy(v);
}
static void catalogue_validation(void)
{
    rtcp_vehicle_characteristic_t chars[RTCP_VEHICLE_MAX_CHARACTERISTICS];
    for (size_t i = 0; i < RTCP_VEHICLE_MAX_CHARACTERISTICS; ++i) {
        chars[i] = spec((int64_t)(10*i+3), RTCP_VEHICLE_INT32, RTCP_VEHICLE_CAR);
        chars[i].accessory_iid = (int64_t)(10*i+1); chars[i].service_iid = (int64_t)(10*i+2);
        chars[i].has_range = true; chars[i].minimum = -10; chars[i].maximum = 10; chars[i].step = 1;
        chars[i].has_units = true; chars[i].units = 151;
        chars[i].has_initial = true; chars[i].initial = number(RTCP_VEHICLE_INT32, 0);
    }
    rtcp_vehicle_config_t config = {.struct_size = sizeof(config), .plugin_name = "catalogue",
        .characteristics = chars, .count = RTCP_VEHICLE_MAX_CHARACTERISTICS};
    rtcp_vehicle_t *v;
    assert(rtcp_vehicle_create(&config, &v) == RTCP_OK);
    size_t length;
    assert(rtcp_vehicle_info(v, response, sizeof(response), &length) == RTCP_OK);
    assert(rtcp_opack_decode(response, length, opened, 512, &opened_count) == RTCP_OPACK_OK);
    rtcp_vehicle_destroy(v);
    chars[1].iid = chars[0].iid;
    assert(rtcp_vehicle_create(&config, &v) == RTCP_EINVAL && !v);
    chars[1].iid = 13; chars[0].step = 0;
    assert(rtcp_vehicle_create(&config, &v) == RTCP_EINVAL);
    chars[0].step = NAN;
    assert(rtcp_vehicle_create(&config, &v) == RTCP_EINVAL);
    chars[0].step = 1; chars[0].maximum = 0x1p63;
    assert(rtcp_vehicle_create(&config, &v) == RTCP_EINVAL);
}
static void portable_step_validation(void)
{
    rtcp_vehicle_characteristic_t s = spec(3, RTCP_VEHICLE_UINT64, RTCP_VEHICLE_SHARED);
    s.has_range = true; s.minimum = 0; s.maximum = 0x1p62; s.step = 2;
    rtcp_vehicle_config_t config = {.struct_size = sizeof(config), .plugin_name = "steps",
        .characteristics = &s, .count = 1};
    rtcp_vehicle_t *v;
    assert(rtcp_vehicle_create(&config, &v) == RTCP_OK);
    rtcp_vehicle_value_t value = number(RTCP_VEHICLE_UINT64, (INT64_C(1)<<60)+1);
    assert(rtcp_vehicle_set(v, 3, &value) == RTCP_EINVAL);
    ++value.as.integer; assert(rtcp_vehicle_set(v, 3, &value) == RTCP_OK);
    value.as.integer = (INT64_C(1)<<62)+1;
    assert(rtcp_vehicle_set(v, 3, &value) == RTCP_EINVAL);
    rtcp_vehicle_destroy(v);
    s.format = RTCP_VEHICLE_INT32; s.minimum = INT32_MIN; s.maximum = INT32_MAX; s.step = 3;
    assert(rtcp_vehicle_create(&config, &v) == RTCP_OK);
    value = number(RTCP_VEHICLE_INT32, INT32_MAX);
    assert(rtcp_vehicle_set(v, 3, &value) == RTCP_OK);
    --value.as.integer; assert(rtcp_vehicle_set(v, 3, &value) == RTCP_EINVAL);
    rtcp_vehicle_destroy(v);
    s.format = RTCP_VEHICLE_FLOAT; s.minimum = -1; s.maximum = 1; s.step = 0.1;
    assert(rtcp_vehicle_create(&config, &v) == RTCP_OK);
    value.format = RTCP_VEHICLE_FLOAT; value.as.real = 0.3;
    assert(rtcp_vehicle_set(v, 3, &value) == RTCP_OK);
    value.as.real = 0.35; assert(rtcp_vehicle_set(v, 3, &value) == RTCP_EINVAL);
    rtcp_vehicle_destroy(v);
}
int main(void)
{
    config_fixture(); subscriptions_and_transactions(); writes_and_policy();
    malformed_and_limits(); catalogue_validation(); portable_step_validation();
    puts("Vehicle State: LIVI fixture, catalogue, ownership/ranges, rollback, subscriptions, copied values, notifications, malformed input passed");
    return 0;
}
