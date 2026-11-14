/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_rtsp_codec.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const uint8_t request_one[] =
    "SETUP /12345678 RTSP/1.0\r\n"
    "CSeq: 19\r\n"
    "Content-Type: application/x-apple-binary-plist\r\n"
    "content-length: 4\r\n\r\n";
static const uint8_t binary_body[] = {0, '\r', '\n', 0xff};
static const uint8_t request_two[] = "OPTIONS * RTSP/1.0\r\nCSeq: 20\r\n\r\n";

static bool contains(const uint8_t *haystack, size_t haystack_len,
                     const char *needle, size_t needle_len)
{
    if (needle_len > haystack_len) return false;
    for (size_t i = 0; i <= haystack_len - needle_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return true;
    }
    return false;
}

int main(void)
{
    uint8_t combined[sizeof(request_one) + sizeof(binary_body) + sizeof(request_two)];
    size_t used = 0;
    memcpy(combined + used, request_one, sizeof(request_one) - 1U);
    used += sizeof(request_one) - 1U;
    memcpy(combined + used, binary_body, sizeof(binary_body));
    used += sizeof(binary_body);
    memcpy(combined + used, request_two, sizeof(request_two) - 1U);
    used += sizeof(request_two) - 1U;

    airplay_rtsp_request_t request = {0};
    assert(airplay_rtsp_parse_request(combined, 9, &request) == AIRPLAY_RTSP_PARSE_INCOMPLETE);
    assert(airplay_rtsp_parse_request(combined, used, &request) == AIRPLAY_RTSP_PARSE_COMPLETE);
    assert(strcmp(request.method, "SETUP") == 0);
    assert(strcmp(request.path, "/12345678") == 0);
    assert(strcmp(request.cseq, "19") == 0);
    assert(strcmp(request.headers[1].name, "content-type") == 0);
    assert(request.body_len == sizeof(binary_body));
    assert(memcmp(request.body, binary_body, sizeof(binary_body)) == 0);

    uint8_t response[256];
    size_t response_len = 0;
    static const uint8_t response_body[] = {1, 2, 0, 4};
    assert(airplay_rtsp_build_response(&request, 200, "OK",
                                       "application/x-apple-binary-plist",
                                       response_body, sizeof(response_body),
                                       response, sizeof(response), &response_len) == 0);
    assert(response_len > sizeof(response_body));
    assert(contains(response, response_len, "RTSP/1.0 200 OK\r\n", 17));
    assert(contains(response, response_len, "CSeq: 19\r\n", 10));
    assert(memcmp(response + response_len - sizeof(response_body),
                  response_body, sizeof(response_body)) == 0);

    size_t second_len = used - request.consumed;
    assert(airplay_rtsp_parse_request(combined + request.consumed, second_len, &request) ==
           AIRPLAY_RTSP_PARSE_COMPLETE);
    assert(strcmp(request.method, "OPTIONS") == 0 && strcmp(request.cseq, "20") == 0);

    static const uint8_t oversized[] =
        "POST /x RTSP/1.0\r\nContent-Length: 32769\r\n\r\n";
    assert(airplay_rtsp_parse_request(oversized, sizeof(oversized) - 1U, &request) ==
           AIRPLAY_RTSP_PARSE_TOO_LARGE);
    static const uint8_t duplicate[] =
        "POST /x RTSP/1.0\r\nContent-Length: 0\r\ncontent-length: 0\r\n\r\n";
    assert(airplay_rtsp_parse_request(duplicate, sizeof(duplicate) - 1U, &request) ==
           AIRPLAY_RTSP_PARSE_INVALID);
    static const uint8_t embedded_nul_protocol[] =
        "OPTIONS * RTSP/1.0\0junk\r\nCSeq: 21\r\n\r\n";
    assert(airplay_rtsp_parse_request(embedded_nul_protocol,
                                      sizeof(embedded_nul_protocol) - 1U, &request) ==
           AIRPLAY_RTSP_PARSE_INVALID);
    static const uint8_t missing_cseq[] = "OPTIONS * RTSP/1.0\r\n\r\n";
    assert(airplay_rtsp_parse_request(missing_cseq, sizeof(missing_cseq) - 1U, &request) ==
           AIRPLAY_RTSP_PARSE_INVALID);
    size_t failed_response_len = 123;
    assert(airplay_rtsp_build_response(&request, 99, "Invalid", NULL, NULL, 0,
                                       response, sizeof(response), &failed_response_len) == -1);
    assert(failed_response_len == 0);
    puts("airplay rtsp codec host test passed");
    return 0;
}
