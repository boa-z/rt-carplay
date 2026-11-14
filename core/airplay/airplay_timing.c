/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_timing.h"

static uint64_t read_be64(const uint8_t *bytes)
{
    uint64_t value = 0U;
    for (size_t i = 0U; i < 8U; ++i) value = (value << 8U) | bytes[i];
    return value;
}

static void write_be64(uint8_t *bytes, uint64_t value)
{
    for (size_t i = 0U; i < 8U; ++i) {
        bytes[7U - i] = (uint8_t)value;
        value >>= 8U;
    }
}

static bool fixed_delta(uint64_t left, uint64_t right, int64_t *delta)
{
    uint64_t difference = left - right;
    if (difference <= INT64_MAX) {
        *delta = (int64_t)difference;
        return true;
    }
    uint64_t magnitude = (~difference) + 1U;
    if (magnitude > INT64_MAX) return false;
    *delta = -(int64_t)magnitude;
    return true;
}

static bool valid_packet(const uint8_t *packet, size_t length, uint8_t type)
{
    return packet != NULL && length == AIRPLAY_TIMING_PACKET_BYTES &&
           (packet[0] >> 6U) == 2U && packet[1] == type &&
           packet[2] == 0U && packet[3] == 7U;
}

bool airplay_timing_build_request(uint64_t transmit_timestamp,
                                  uint8_t *output,
                                  size_t output_capacity,
                                  size_t *output_len)
{
    if (output_len != NULL) *output_len = 0U;
    if (output == NULL || output_len == NULL ||
        output_capacity < AIRPLAY_TIMING_PACKET_BYTES) return false;
    for (size_t i = 0U; i < AIRPLAY_TIMING_PACKET_BYTES; ++i) output[i] = 0U;
    output[0] = 0x80U;
    output[1] = AIRPLAY_TIMING_PT_REQUEST;
    output[3] = 7U;
    write_be64(output + 24U, transmit_timestamp);
    *output_len = AIRPLAY_TIMING_PACKET_BYTES;
    return true;
}

bool airplay_timing_build_response(const uint8_t *request,
                                   size_t request_len,
                                   uint64_t receive_timestamp,
                                   uint64_t transmit_timestamp,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   size_t *output_len)
{
    if (output_len != NULL) *output_len = 0U;
    if (!valid_packet(request, request_len, AIRPLAY_TIMING_PT_REQUEST) ||
        output == NULL || output_len == NULL ||
        output_capacity < AIRPLAY_TIMING_PACKET_BYTES) return false;
    for (size_t i = 0U; i < AIRPLAY_TIMING_PACKET_BYTES; ++i) output[i] = 0U;
    output[0] = 0x80U;
    output[1] = AIRPLAY_TIMING_PT_RESPONSE;
    output[3] = 7U;
    for (size_t i = 0U; i < 8U; ++i) output[8U + i] = request[24U + i];
    write_be64(output + 16U, receive_timestamp);
    write_be64(output + 24U, transmit_timestamp);
    *output_len = AIRPLAY_TIMING_PACKET_BYTES;
    return true;
}

bool airplay_timing_parse_response(const uint8_t *response,
                                   size_t response_len,
                                   uint64_t expected_transmit_timestamp,
                                   uint64_t receive_timestamp,
                                   double *round_trip_seconds,
                                   double *clock_offset_seconds)
{
    if (!valid_packet(response, response_len, AIRPLAY_TIMING_PT_RESPONSE) ||
        round_trip_seconds == NULL || clock_offset_seconds == NULL ||
        read_be64(response + 8U) != expected_transmit_timestamp) return false;
    uint64_t t2 = read_be64(response + 16U);
    uint64_t t3 = read_be64(response + 24U);
    int64_t t2_minus_t1 = 0;
    int64_t t3_minus_t4 = 0;
    int64_t t4_minus_t1 = 0;
    int64_t t3_minus_t2 = 0;
    if (!fixed_delta(t2, expected_transmit_timestamp, &t2_minus_t1) ||
        !fixed_delta(t3, receive_timestamp, &t3_minus_t4) ||
        !fixed_delta(receive_timestamp, expected_transmit_timestamp, &t4_minus_t1) ||
        !fixed_delta(t3, t2, &t3_minus_t2)) return false;
    const double fixed_to_seconds = 1.0 / 4294967296.0;
    *round_trip_seconds = ((double)t4_minus_t1 - (double)t3_minus_t2) * fixed_to_seconds;
    *clock_offset_seconds =
        ((double)t2_minus_t1 + (double)t3_minus_t4) * 0.5 * fixed_to_seconds;
    return *round_trip_seconds >= 0.0;
}
