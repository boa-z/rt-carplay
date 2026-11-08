/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_tlv8.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t long_value[512];
    for (size_t i = 0; i < sizeof(long_value); ++i) long_value[i] = (uint8_t)i;
    static const uint8_t state[] = {2};
    static const uint8_t identifier_a[] = {'a'};
    static const uint8_t identifier_b[] = {'b', 'c'};
    const airplay_tlv8_item_t items[] = {
        {.type = 6, .value = state, .value_len = sizeof(state)},
        {.type = 3, .value = long_value, .value_len = sizeof(long_value)},
        {.type = 1, .value = identifier_a, .value_len = sizeof(identifier_a)},
        {.type = 1, .value = identifier_b, .value_len = sizeof(identifier_b)},
    };
    uint8_t encoded[600];
    size_t encoded_len = 0;
    assert(airplay_tlv8_encode(items, sizeof(items) / sizeof(items[0]),
                               encoded, sizeof(encoded), &encoded_len) == AIRPLAY_TLV8_OK);

    uint8_t decoded[512];
    size_t decoded_len = 0;
    assert(airplay_tlv8_get(encoded, encoded_len, 3, decoded,
                            sizeof(decoded), &decoded_len) == AIRPLAY_TLV8_OK);
    assert(decoded_len == sizeof(long_value));
    assert(memcmp(decoded, long_value, sizeof(long_value)) == 0);
    assert(airplay_tlv8_get(encoded, encoded_len, 1, decoded,
                            sizeof(decoded), &decoded_len) == AIRPLAY_TLV8_OK);
    assert(decoded_len == sizeof(identifier_b));
    assert(memcmp(decoded, identifier_b, sizeof(identifier_b)) == 0);
    assert(airplay_tlv8_get(encoded, encoded_len, 9, decoded,
                            sizeof(decoded), &decoded_len) == AIRPLAY_TLV8_NOT_FOUND);

    assert(airplay_tlv8_get(encoded, encoded_len, 3, decoded, 400,
                            &decoded_len) == AIRPLAY_TLV8_TOO_LARGE);
    assert(airplay_tlv8_get(encoded, encoded_len - 1U, 3, decoded,
                            sizeof(decoded), &decoded_len) == AIRPLAY_TLV8_MALFORMED);
    static const uint8_t truncated[] = {1, 3, 0xaa};
    assert(airplay_tlv8_get(truncated, sizeof(truncated), 1, decoded,
                            sizeof(decoded), &decoded_len) == AIRPLAY_TLV8_MALFORMED);
    uint8_t oversized_input[AIRPLAY_TLV8_MAX_MESSAGE_BYTES + 1U] = {0};
    assert(airplay_tlv8_get(oversized_input, sizeof(oversized_input), 1, decoded,
                            sizeof(decoded), &decoded_len) == AIRPLAY_TLV8_TOO_LARGE);

    uint8_t short_output[2];
    size_t short_len = 0;
    assert(airplay_tlv8_encode(items, sizeof(items) / sizeof(items[0]),
                               short_output, sizeof(short_output), &short_len) ==
           AIRPLAY_TLV8_TOO_LARGE);
    const airplay_tlv8_item_t invalid = {.type = 0xff, .value = NULL, .value_len = 0};
    assert(airplay_tlv8_encode(&invalid, 1, encoded, sizeof(encoded), &encoded_len) ==
           AIRPLAY_TLV8_INVALID_ARGUMENT);
    puts("airplay tlv8 host test passed");
    return 0;
}
