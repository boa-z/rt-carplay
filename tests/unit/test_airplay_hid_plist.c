/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_hid_plist.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t read_be64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
    return value;
}

static size_t string_object(const uint8_t *plist, size_t offset,
                            const char *expected, size_t expected_len)
{
    assert((plist[offset] >> 4) == 5);
    size_t length = plist[offset] & 0x0f;
    assert(length == expected_len);
    assert(memcmp(plist + offset + 1, expected, length) == 0);
    return offset + 1 + length;
}

int main(void)
{
    const uint8_t report[] = {0, 1, 9, 0, 7, 0, 1, 0, 0, 0, 0, 0};
    uint8_t plist[160];
    size_t length = 0;
    assert(airplay_hid_plist_encode(0x2a2a2a2aU, report, sizeof(report),
                                    plist, sizeof(plist), &length));
    assert(length == 110);
    assert(memcmp(plist, "bplist00", 8) == 0);

    size_t trailer = length - 32;
    size_t offset_table = (size_t)read_be64(plist + trailer + 24);
    assert(read_be64(plist + trailer + 8) == 7);
    assert(read_be64(plist + trailer + 16) == 0);
    assert(plist[trailer + 6] == 1 && plist[trailer + 7] == 1);
    assert(plist[8] == 0xd3);
    assert(memcmp(plist + 9, (uint8_t[]){1, 2, 3, 4, 5, 6}, 6) == 0);

    size_t offsets[7];
    for (size_t i = 0; i < 7; ++i) offsets[i] = plist[offset_table + i];
    string_object(plist, offsets[1], "type", 4);
    string_object(plist, offsets[2], "uuid", 4);
    string_object(plist, offsets[3], "hidReport", 9);
    string_object(plist, offsets[4], "hidSendReport", 13);
    string_object(plist, offsets[5], "2a2a2a2a", 8);
    assert(plist[offsets[6]] == 0x4c);
    assert(memcmp(plist + offsets[6] + 1, report, sizeof(report)) == 0);

    size_t rejected_length = 123;
    assert(!airplay_hid_plist_encode(1, report, sizeof(report), plist, 20, &rejected_length));
    assert(rejected_length == 0);
    puts("airplay hid plist host test passed");
    return 0;
}
