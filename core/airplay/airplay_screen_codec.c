/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_screen_codec.h"

#include <string.h>

#define AIRPLAY_SCREEN_OPCODE_OFFSET 4U
#define AIRPLAY_SCREEN_SENDER_TIME_OFFSET 8U

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_le64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8) | bytes[index];
    }
    return value;
}

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void write_start_code(uint8_t *bytes)
{
    bytes[0] = 0;
    bytes[1] = 0;
    bytes[2] = 0;
    bytes[3] = 1;
}

bool airplay_screen_parse_header(const uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES],
                                 size_t received_body_bytes,
                                 airplay_screen_header_info_t *out_info)
{
    if (header == NULL || out_info == NULL || received_body_bytes > UINT32_MAX) {
        return false;
    }
    uint32_t body_bytes = read_le32(header);
    uint8_t opcode = header[AIRPLAY_SCREEN_OPCODE_OFFSET];
    if (body_bytes != received_body_bytes) {
        return false;
    }

    uint64_t ntp = read_le64(header + AIRPLAY_SCREEN_SENDER_TIME_OFFSET);
    uint64_t seconds_ns = (ntp >> 32) * UINT64_C(1000000000);
    uint64_t fractional_ns = ((ntp & UINT64_C(0xffffffff)) * UINT64_C(1000000000)) >> 32;
    out_info->body_bytes = body_bytes;
    out_info->opcode = opcode;
    out_info->sender_time_ns = seconds_ns + fractional_ns;
    return true;
}

bool airplay_screen_convert_length_prefixed_to_annex_b(uint8_t *data, size_t len)
{
    if (data == NULL || len < 4) {
        return false;
    }
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        return true;
    }

    size_t offset = 0;
    while (offset < len) {
        if (len - offset < 4) {
            return false;
        }
        uint32_t nal_len = ((uint32_t)data[offset] << 24) |
                           ((uint32_t)data[offset + 1] << 16) |
                           ((uint32_t)data[offset + 2] << 8) |
                           data[offset + 3];
        offset += 4;
        if (nal_len == 0 || (size_t)nal_len > len - offset) {
            return false;
        }
        offset += nal_len;
    }

    offset = 0;
    while (offset < len) {
        uint32_t nal_len = ((uint32_t)data[offset] << 24) |
                           ((uint32_t)data[offset + 1] << 16) |
                           ((uint32_t)data[offset + 2] << 8) |
                           data[offset + 3];
        write_start_code(data + offset);
        offset += 4U + nal_len;
    }
    return true;
}

static bool append_nal(uint8_t *out, size_t capacity, size_t *used,
                       const uint8_t *nal, size_t nal_len)
{
    if (nal_len == 0 || *used > capacity || nal_len + 4U > capacity - *used) {
        return false;
    }
    write_start_code(out + *used);
    memcpy(out + *used + 4U, nal, nal_len);
    *used += nal_len + 4U;
    return true;
}

bool airplay_screen_extract_avcc_to_annex_b(const uint8_t *config_body,
                                            size_t config_body_len,
                                            uint8_t *out,
                                            size_t out_capacity,
                                            size_t *out_len)
{
    static const uint8_t marker[] = {'a', 'v', 'c', 'C'};
    if (config_body == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    *out_len = 0;
    size_t record_offset = SIZE_MAX;
    for (size_t i = 0; i + sizeof(marker) <= config_body_len; ++i) {
        if (memcmp(config_body + i, marker, sizeof(marker)) == 0) {
            record_offset = i + sizeof(marker);
            break;
        }
    }
    if (record_offset == SIZE_MAX) {
        /* DiPlay also accepts a bare AVCDecoderConfigurationRecord when the
         * surrounding VideoConfig payload has no avcC atom marker. Do not
         * apply this fallback to a payload explicitly marked as HEVC. */
        static const uint8_t hevc_marker[] = {'h', 'v', 'c', 'C'};
        for (size_t i = 0; i + sizeof(hevc_marker) <= config_body_len; ++i) {
            if (memcmp(config_body + i, hevc_marker, sizeof(hevc_marker)) == 0) {
                return false;
            }
        }
        record_offset = 0U;
    }
    if (record_offset > config_body_len || config_body_len - record_offset < 7U) {
        return false;
    }

    const uint8_t *record = config_body + record_offset;
    size_t record_len = config_body_len - record_offset;
    if (record[0] != 1 || (record[4] & 0xfcU) != 0xfcU ||
        (record[5] & 0xe0U) != 0xe0U) {
        return false;
    }
    size_t offset = 5U;
    uint8_t sps_count = record[offset++] & 0x1fU;
    if (sps_count == 0) {
        return false;
    }
    size_t used = 0;
    for (uint8_t i = 0; i < sps_count; ++i) {
        if (record_len - offset < 2U) return false;
        uint16_t nal_len = read_be16(record + offset);
        offset += 2U;
        if (nal_len == 0 || nal_len > record_len - offset ||
            (record[offset] & 0x1fU) != 7U ||
            !append_nal(out, out_capacity, &used, record + offset, nal_len)) {
            return false;
        }
        offset += nal_len;
    }
    if (offset >= record_len) return false;
    uint8_t pps_count = record[offset++];
    if (pps_count == 0) return false;
    for (uint8_t i = 0; i < pps_count; ++i) {
        if (record_len - offset < 2U) return false;
        uint16_t nal_len = read_be16(record + offset);
        offset += 2U;
        if (nal_len == 0 || nal_len > record_len - offset ||
            (record[offset] & 0x1fU) != 8U ||
            !append_nal(out, out_capacity, &used, record + offset, nal_len)) {
            return false;
        }
        offset += nal_len;
    }
    if (used == 0) return false;
    *out_len = used;
    return true;
}
