/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_TIMING_PACKET_BYTES 32U
#define AIRPLAY_TIMING_PT_REQUEST 210U
#define AIRPLAY_TIMING_PT_RESPONSE 211U

/* Pure helpers for the fixed-size AirPlay timing exchange packets. */
bool airplay_timing_build_request(uint64_t transmit_timestamp,
                                  uint8_t *output,
                                  size_t output_capacity,
                                  size_t *output_len);
bool airplay_timing_build_response(const uint8_t *request,
                                   size_t request_len,
                                   uint64_t receive_timestamp,
                                   uint64_t transmit_timestamp,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   size_t *output_len);
bool airplay_timing_parse_response(const uint8_t *response,
                                   size_t response_len,
                                   uint64_t expected_transmit_timestamp,
                                   uint64_t receive_timestamp,
                                   double *round_trip_seconds,
                                   double *clock_offset_seconds);
