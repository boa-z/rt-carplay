/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VEHICLE_TEST_WIRE_H
#define VEHICLE_TEST_WIRE_H
#include "rtcp/vehicle.h"
#include "rtcp/opack.h"
#include "rtcp/rtcp.h"
#include <assert.h>
#include <string.h>

/* Synthetic catalogue identifiers, not production vehicle protocol mappings. */
static rtcp_vehicle_t *vehicle_test_create(void)
{
    rtcp_vehicle_characteristic_t spec = {
        .accessory_iid = 1, .accessory_type = 100, .service_iid = 2, .service_type = 200,
        .iid = 3, .type = 1003, .format = RTCP_VEHICLE_UINT32,
        .owner = RTCP_VEHICLE_SHARED, .mutable_value = true, .has_initial = true,
        .initial = {.format = RTCP_VEHICLE_UINT32, .as.integer = 7}};
    rtcp_vehicle_config_t config = {.struct_size = sizeof(config), .plugin_name = "test",
        .characteristics = &spec, .count = 1};
    rtcp_vehicle_t *v = NULL;
    assert(rtcp_vehicle_create(&config, &v) == RTCP_OK); return v;
}
static size_t vehicle_test_request(const char *cmd, bool write, int64_t value, uint8_t *out, size_t cap)
{
    rtcp_opack_node_t n[10] = {0}; uint8_t inner[512]; size_t length;
    n[0] = (rtcp_opack_node_t){.type = RTCP_OPACK_DICT, .count = 3, .end = write ? 9 : 8};
    const char *texts[] = {"command", cmd, "transactionID", "values"};
    const unsigned at[] = {1, 2, 3, 5};
    for (unsigned i = 0; i < 4; ++i) {
        n[at[i]] = (rtcp_opack_node_t){.type = RTCP_OPACK_STRING, .end = (uint16_t)(at[i]+1)};
        n[at[i]].value.blob.bytes = (const uint8_t *)texts[i];
        n[at[i]].value.blob.length = strlen(texts[i]);
    }
    n[4] = (rtcp_opack_node_t){.type = RTCP_OPACK_INT, .end = 5, .value.integer = -7};
    n[6] = (rtcp_opack_node_t){.type = write ? RTCP_OPACK_DICT : RTCP_OPACK_ARRAY,
        .count = 1, .end = write ? 9 : 8};
    n[7] = (rtcp_opack_node_t){.type = RTCP_OPACK_INT, .end = 8, .value.integer = 3};
    n[8] = (rtcp_opack_node_t){.type = RTCP_OPACK_INT, .end = 9, .value.integer = value};
    assert(rtcp_opack_encode(n, write ? 9 : 8, inner, sizeof(inner), &length) == RTCP_OPACK_OK);
    n[0] = (rtcp_opack_node_t){.type = RTCP_OPACK_DICT, .count = 2, .end = 5};
    n[1].value.blob.bytes = (const uint8_t *)"pluginID"; n[1].value.blob.length = 8;
    n[2] = (rtcp_opack_node_t){.type = RTCP_OPACK_INT, .end = 3, .value.integer = 0};
    n[3].value.blob.bytes = (const uint8_t *)"pluginData"; n[3].value.blob.length = 10;
    n[4] = (rtcp_opack_node_t){.type = RTCP_OPACK_DATA, .end = 5};
    n[4].value.blob.bytes = inner; n[4].value.blob.length = length;
    assert(rtcp_opack_encode(n, 5, out, cap, &length) == RTCP_OPACK_OK); return length;
}
static void vehicle_test_reply(const uint8_t *body, size_t length, const char *cmd,
    bool transaction, int64_t expected_value)
{
    rtcp_opack_node_t n[128]; size_t count;
    assert(rtcp_opack_decode(body, length, n, 128, &count) == RTCP_OPACK_OK);
    size_t data = rtcp_opack_dict_get(n, count, 0, "pluginData"); assert(data != SIZE_MAX);
    const uint8_t *inner = n[data].value.blob.bytes; size_t size = n[data].value.blob.length;
    assert(rtcp_opack_decode(inner, size, n, 128, &count) == RTCP_OPACK_OK);
    size_t command = rtcp_opack_dict_get(n, count, 0, "command"); assert(command != SIZE_MAX);
    assert(n[command].type == RTCP_OPACK_STRING && n[command].value.blob.length == strlen(cmd));
    assert(!memcmp(n[command].value.blob.bytes, cmd, strlen(cmd)));
    if (transaction) {
        size_t id = rtcp_opack_dict_get(n, count, 0, "transactionID");
        assert(id != SIZE_MAX && n[id].type == RTCP_OPACK_INT && n[id].value.integer == -7);
    }
    if (expected_value >= 0) {
        size_t values = rtcp_opack_dict_get(n, count, 0, "values");
        assert(values != SIZE_MAX && n[values].type == RTCP_OPACK_DICT && n[values].count == 1);
        assert(n[values+1].value.integer == 3 && n[values+2].value.integer == expected_value);
    }
}
#endif
