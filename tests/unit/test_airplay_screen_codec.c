/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_screen_codec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *out, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) out[i] = (uint8_t)(value >> (8U * i));
}

int main(void)
{
    uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES] = {0};
    put_le32(header, 5);
    header[4] = 0;
    put_le64(header + 8, (UINT64_C(2) << 32) | UINT64_C(0x80000000));
    airplay_screen_header_info_t info;
    assert(airplay_screen_parse_header(header, 5, &info));
    assert(info.body_bytes == 5 && info.opcode == 0);
    assert(info.sender_time_ns == UINT64_C(2500000000));
    assert(!airplay_screen_parse_header(header, 4, &info));
    header[4] = 7;
    assert(airplay_screen_parse_header(header, 5, &info));
    assert(info.opcode == 7U);
    put_le32(header, 0U);
    assert(airplay_screen_parse_header(header, 0U, &info));
    assert(info.opcode == 7U && info.body_bytes == 0U);
    assert(!airplay_screen_parse_header(header, 1U, &info));

    uint8_t frame[] = {0, 0, 0, 2, 0x65, 0xaa, 0, 0, 0, 1, 0x41};
    assert(airplay_screen_convert_length_prefixed_to_annex_b(frame, sizeof(frame)));
    static const uint8_t expected[] = {0, 0, 0, 1, 0x65, 0xaa, 0, 0, 0, 1, 0x41};
    assert(memcmp(frame, expected, sizeof(expected)) == 0);
    uint8_t malformed[] = {0, 0, 0, 8, 0x65};
    uint8_t malformed_before[sizeof(malformed)];
    memcpy(malformed_before, malformed, sizeof(malformed));
    assert(!airplay_screen_convert_length_prefixed_to_annex_b(malformed, sizeof(malformed)));
    assert(memcmp(malformed, malformed_before, sizeof(malformed)) == 0);

    const uint8_t avcc[] = {
        0, 0, 0, 0, 'a', 'v', 'c', 'C',
        1, 100, 0, 31, 0xff, 0xe1, 0, 3, 0x67, 0x64, 0x00, 1, 0, 2, 0x68, 0xee,
    };
    uint8_t config[32];
    size_t config_len = 0;
    assert(airplay_screen_extract_avcc_to_annex_b(avcc, sizeof(avcc),
                                                  config, sizeof(config), &config_len));
    static const uint8_t expected_config[] = {
        0, 0, 0, 1, 0x67, 0x64, 0x00,
        0, 0, 0, 1, 0x68, 0xee,
    };
    assert(config_len == sizeof(expected_config));
    assert(memcmp(config, expected_config, sizeof(expected_config)) == 0);
    assert(!airplay_screen_extract_avcc_to_annex_b(avcc, sizeof(avcc), config, 4, &config_len));

    puts("airplay_screen codec host test passed");
    return 0;
}
