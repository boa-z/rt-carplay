/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_SCREEN_HEADER_BYTES 128U

typedef struct {
    uint32_t body_bytes;
    uint8_t opcode;
    uint64_t sender_time_ns;
} airplay_screen_header_info_t;

/* Validates framing, including messages with opcodes the media decoder ignores. */
bool airplay_screen_parse_header(const uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES],
                                 size_t received_body_bytes,
                                 airplay_screen_header_info_t *out_info);
bool airplay_screen_convert_length_prefixed_to_annex_b(uint8_t *data, size_t len);
bool airplay_screen_extract_avcc_to_annex_b(const uint8_t *config_body,
                                            size_t config_body_len,
                                            uint8_t *out,
                                            size_t out_capacity,
                                            size_t *out_len);
