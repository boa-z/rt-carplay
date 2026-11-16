/* SPDX-License-Identifier: GPL-3.0-or-later
 * Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI).
 * The configRequest wire fixture is from LIVI vehicle_state/tests.rs.
 */
#include "rtcp/opack.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static rtcp_opack_node_t nodes[RTCP_OPACK_MAX_NODES], decoded[RTCP_OPACK_MAX_NODES];
static uint8_t encoded[RTCP_OPACK_MAX_BYTES];

static void invalid(const uint8_t *bytes, size_t length)
{
    size_t count = 99;
    assert(rtcp_opack_decode(bytes, length, nodes, RTCP_OPACK_MAX_NODES, &count) != RTCP_OPACK_OK);
    assert(count == 0);
}

static void fixture(void)
{
    /* pluginID=0, pluginData=OPACK(configRequest, signed transactionID). */
    static const uint8_t request[] = {
        0xe2, 0x48, 0x70, 0x6c, 0x75, 0x67, 0x69, 0x6e, 0x49, 0x44, 0x08,
        0x4a, 0x70, 0x6c, 0x75, 0x67, 0x69, 0x6e, 0x44, 0x61, 0x74, 0x61, 0x91, 0x2e,
        0xe2, 0x47, 0x63, 0x6f, 0x6d, 0x6d, 0x61, 0x6e, 0x64, 0x4d, 0x63, 0x6f, 0x6e,
        0x66, 0x69, 0x67, 0x52, 0x65, 0x71, 0x75, 0x65, 0x73, 0x74, 0x4d, 0x74, 0x72,
        0x61, 0x6e, 0x73, 0x61, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x49, 0x44, 0x33,
        0xb9, 0x60, 0xc3, 0x62, 0xb5, 0x2e, 0xd4, 0xf2
    };
    size_t count, length;
    assert(rtcp_opack_decode(request, sizeof(request), nodes, RTCP_OPACK_MAX_NODES, &count) == RTCP_OPACK_OK);
    assert(count == 5 && nodes[0].count == 2);
    size_t id = rtcp_opack_dict_get(nodes, count, 0, "pluginID");
    size_t data = rtcp_opack_dict_get(nodes, count, 0, "pluginData");
    assert(id != SIZE_MAX && nodes[id].type == RTCP_OPACK_INT && nodes[id].value.integer == 0);
    assert(data != SIZE_MAX && nodes[data].type == RTCP_OPACK_DATA);
    assert(rtcp_opack_encode(nodes, count, encoded, sizeof(encoded), &length) == RTCP_OPACK_OK);
    assert(length == sizeof(request) && memcmp(encoded, request, length) == 0);
    assert(rtcp_opack_decode(nodes[data].value.blob.bytes, nodes[data].value.blob.length,
                             decoded, RTCP_OPACK_MAX_NODES, &count) == RTCP_OPACK_OK);
    id = rtcp_opack_dict_get(decoded, count, 0, "transactionID");
    assert(id != SIZE_MAX && decoded[id].value.integer == -INT64_C(949082264887402311));
    for (size_t n = 0; n < sizeof(request); ++n) invalid(request, n);
}

static void references(void)
{
    static const uint8_t wire[] = { 0xdf, 0x41, 'a', 0xa0, 0xc1, 0, 0x30, 0xff, 0xa1, 3 };
    size_t count, length;
    assert(rtcp_opack_decode(wire, sizeof(wire), nodes, 512, &count) == RTCP_OPACK_OK);
    assert(count == 6 && nodes[0].count == 5);
    assert(nodes[2].value.blob.bytes == nodes[1].value.blob.bytes);
    assert(nodes[4].value.integer == -1 && nodes[5].value.integer == -1);
    assert(rtcp_opack_encode(nodes, count, encoded, sizeof(encoded), &length) == RTCP_OPACK_OK);
    assert(rtcp_opack_decode(encoded, length, decoded, 512, &count) == RTCP_OPACK_OK);
    assert(decoded[5].value.integer == -1);
    static const uint8_t bad_ref[] = { 0xa0 };
    static const uint8_t short_ref[] = { 0xc2, 0 };
    static const uint8_t odd_dict[] = { 0xef, 0x41, 'a', 3 };
    static const uint8_t bad_utf8[] = { 0x42, 0xc0, 0x80 };
    static const uint8_t trailing[] = { 4, 4 };
    static const uint8_t huge[] = { 0x94, 0xff, 0xff, 0xff, 0xff };
    invalid(bad_ref, sizeof(bad_ref));
    invalid(short_ref, sizeof(short_ref));
    invalid(odd_dict, sizeof(odd_dict));
    invalid(bad_utf8, sizeof(bad_utf8));
    invalid(trailing, sizeof(trailing));
    invalid(huge, sizeof(huge));
    uint8_t depth[RTCP_OPACK_MAX_DEPTH + 2];
    memset(depth, 0xd1, sizeof(depth));
    depth[sizeof(depth) - 1] = 4;
    invalid(depth, sizeof(depth));
    static const uint8_t dup[] = { 0xe2, 0x41, 'a', 8, 0x41, 'a', 9 };
    assert(rtcp_opack_decode(dup, sizeof(dup), nodes, 512, &count) == RTCP_OPACK_OK);
    assert(rtcp_opack_dict_get(nodes, count, 0, "a") == SIZE_MAX);
}

static void numbers_and_bounds(void)
{
    const int64_t values[] = { INT64_MIN, -32769, -32768, -129, -128, -1, 0, 39,
                               40, 127, 128, 32767, 32768, INT32_MAX, INT64_MAX };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    {
        rtcp_opack_node_t n = { .type = RTCP_OPACK_INT, .end = 1 };
        n.value.integer = values[i];
        size_t count, length;
        assert(rtcp_opack_encode(&n, 1, encoded, sizeof(encoded), &length) == RTCP_OPACK_OK);
        assert(rtcp_opack_decode(encoded, length, decoded, 512, &count) == RTCP_OPACK_OK);
        assert(count == 1 && decoded[0].value.integer == values[i]);
        size_t output = 99;
        assert(rtcp_opack_encode(&n, 1, encoded, length - 1, &output) == RTCP_OPACK_OUTPUT_TOO_SMALL);
        assert(output == 0);
    }
    rtcp_opack_node_t n = { .type = RTCP_OPACK_REAL, .end = 1 };
    n.value.real = NAN;
    size_t length, count;
    assert(rtcp_opack_encode(&n, 1, encoded, sizeof(encoded), &length) == RTCP_OPACK_INVALID);
    const uint8_t nan[] = { 0x36, 0, 0, 0, 0, 0, 0, 0xf8, 0x7f };
    invalid(nan, sizeof(nan));
    const uint8_t simple[] = { 0xd1, 4 };
    assert(rtcp_opack_decode(simple, sizeof(simple), nodes, 1, &count) == RTCP_OPACK_LIMIT);
    assert(count == 0);
    /* Deterministic malformed-input corpus under ASan/UBSan. */
    uint32_t state = 123;
    uint8_t fuzz[96];
    for (unsigned i = 0; i < 4000; ++i)
    {
        size_t bytes = i % sizeof(fuzz) + 1;
        for (size_t j = 0; j < bytes; ++j)
        {
            state = state * 1664525U + 1013904223U;
            fuzz[j] = (uint8_t)(state >> 24);
        }
        if (rtcp_opack_decode(fuzz, bytes, nodes, 512, &count) == RTCP_OPACK_OK)
        {
            assert(rtcp_opack_encode(nodes, count, encoded, sizeof(encoded), &length) == RTCP_OPACK_OK);
            assert(rtcp_opack_decode(encoded, length, decoded, 512, &count) == RTCP_OPACK_OK);
        }
    }
}

int main(void)
{
    fixture();
    references();
    numbers_and_bounds();
    puts("OPACK: LIVI wire fixture, signed IDs, references, UTF-8, bounds and malformed corpus passed");
    return 0;
}
