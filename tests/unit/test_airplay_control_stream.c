/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_control_stream.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    size_t count;
    size_t body_bytes;
    bool fail;
} fixture_t;

static bool on_request(const airplay_rtsp_request_t *request, void *context)
{
    fixture_t *fixture = context;
    assert(strcmp(request->method, "POST") == 0);
    assert(strcmp(request->protocol, "RTSP/1.0") == 0);
    assert(request->path[0] == '/');
    fixture->count++;
    fixture->body_bytes += request->body_len;
    return !fixture->fail;
}

static void test_fragmentation_coalescing_and_responses(void)
{
    airplay_control_stream_t *stream = NULL;
    assert(airplay_control_stream_create(&stream) == AIRPLAY_CONTROL_STREAM_OK);
    fixture_t fixture = {0};
    static const uint8_t request1[] =
        "POST /command RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 3\r\n\r\none";
    static const uint8_t response[] = "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n";
    static const uint8_t request2[] =
        "POST /command RTSP/1.0\r\nCSeq: 2\r\nContent-Length: 2\r\n\r\ntw";
    uint8_t combined[sizeof(request1) + sizeof(response) + sizeof(request2)];
    size_t offset = 0U;
    memcpy(combined + offset, request1, sizeof(request1) - 1U);
    offset += sizeof(request1) - 1U;
    memcpy(combined + offset, response, sizeof(response) - 1U);
    offset += sizeof(response) - 1U;
    memcpy(combined + offset, request2, sizeof(request2) - 1U);
    offset += sizeof(request2) - 1U;

    for (size_t i = 0U; i < offset; ++i) {
        airplay_control_stream_result_t result = airplay_control_stream_feed(
            stream, combined + i, 1U, on_request, &fixture);
        assert(result == AIRPLAY_CONTROL_STREAM_OK ||
               result == AIRPLAY_CONTROL_STREAM_INCOMPLETE);
    }
    assert(fixture.count == 2U);
    assert(fixture.body_bytes == 5U);
    airplay_control_stream_destroy(stream);
}

static void test_limits_and_callback_failure(void)
{
    airplay_control_stream_t *stream = NULL;
    assert(airplay_control_stream_create(&stream) == AIRPLAY_CONTROL_STREAM_OK);
    fixture_t fixture = {.fail = true};
    static const uint8_t request[] = "POST /event RTSP/1.0\r\nCSeq: 7\r\n\r\n";
    assert(airplay_control_stream_feed(stream, request, sizeof(request) - 1U,
                                       on_request, &fixture) ==
           AIRPLAY_CONTROL_STREAM_CALLBACK_FAILED);
    assert(fixture.count == 1U);
    airplay_control_stream_destroy(stream);

    assert(airplay_control_stream_create(&stream) == AIRPLAY_CONTROL_STREAM_OK);
    static const uint8_t oversized_response[] =
        "RTSP/1.0 200 OK\r\nContent-Length: 2049\r\n\r\n";
    assert(airplay_control_stream_feed(stream, oversized_response,
                                       sizeof(oversized_response) - 1U,
                                       on_request, &fixture) ==
           AIRPLAY_CONTROL_STREAM_TOO_LARGE);
    airplay_control_stream_destroy(stream);
}

int main(void)
{
    test_fragmentation_coalescing_and_responses();
    test_limits_and_callback_failure();
    puts("airplay ControlCipher RTSP stream host test passed");
    return 0;
}
