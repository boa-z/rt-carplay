/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_timing.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_request_and_echo(void)
{
    uint8_t request[AIRPLAY_TIMING_PACKET_BYTES];
    uint8_t response[AIRPLAY_TIMING_PACKET_BYTES];
    size_t request_len = 0U;
    size_t response_len = 0U;
    const uint64_t t1 = 0x0123456789abcdefULL;
    const uint64_t t2 = 0x0123456789abcff0ULL;
    const uint64_t t3 = 0x0123456789abd000ULL;
    const uint64_t t4 = 0x0123456789abd111ULL;
    assert(airplay_timing_build_request(t1, request, sizeof(request), &request_len));
    assert(request_len == sizeof(request));
    assert(request[0] == 0x80U && request[1] == AIRPLAY_TIMING_PT_REQUEST);
    assert(request[2] == 0U && request[3] == 7U);
    assert(airplay_timing_build_response(request, request_len, t2, t3,
                                         response, sizeof(response), &response_len));
    assert(response_len == sizeof(response));
    assert(response[1] == AIRPLAY_TIMING_PT_RESPONSE);
    assert(memcmp(request + 24U, response + 8U, 8U) == 0);
    double rtt = 0.0;
    double offset = 0.0;
    assert(airplay_timing_parse_response(response, response_len, t1, t4,
                                         &rtt, &offset));
    assert(fabs(rtt - 786.0 / 4294967296.0) < 1e-12);
    assert(fabs(offset - 120.0 / 4294967296.0) < 1e-12);
}

static void test_reject_malformed_and_stale_packets(void)
{
    uint8_t request[AIRPLAY_TIMING_PACKET_BYTES];
    uint8_t response[AIRPLAY_TIMING_PACKET_BYTES];
    size_t length = 0U;
    assert(airplay_timing_build_request(100U, request, sizeof(request), &length));
    assert(!airplay_timing_build_response(request, length - 1U, 101U, 102U,
                                          response, sizeof(response), &length));
    assert(airplay_timing_build_response(request, sizeof(request), 101U, 102U,
                                         response, sizeof(response), &length));
    double rtt = 0.0;
    double offset = 0.0;
    assert(!airplay_timing_parse_response(response, length, 99U, 103U,
                                          &rtt, &offset));
    response[3] = 6U;
    assert(!airplay_timing_parse_response(response, length, 100U, 103U,
                                          &rtt, &offset));
}

int main(void)
{
    test_request_and_echo();
    test_reject_malformed_and_stale_packets();
    puts("airplay timing host test passed");
    return 0;
}
