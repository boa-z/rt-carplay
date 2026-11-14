/* SPDX-License-Identifier: GPL-3.0-or-later; command shape follows DiPlay */
#include "airplay_command_plist.h"

#include <string.h>

#define FORCE_KEYFRAME_PLIST_BYTES 65U
#define FORCE_KEYFRAME_OBJECTS 3U
#define FORCE_KEYFRAME_TRAILER_BYTES 32U

bool airplay_command_plist_encode_force_keyframe(uint8_t *output,
                                                 size_t output_capacity,
                                                 size_t *output_len)
{
    static const uint8_t magic[] = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
    static const uint8_t dictionary[] = {0xd1, 0x01, 0x02};
    static const char key[] = "type";
    static const char command[] = "forceKeyFrame";
    static const uint8_t offsets[] = {8U, 11U, 16U};

    if (output_len != NULL) *output_len = 0U;
    if (output == NULL || output_len == NULL ||
        output_capacity < FORCE_KEYFRAME_PLIST_BYTES) {
        return false;
    }

    memcpy(output, magic, sizeof(magic));
    memcpy(output + 8U, dictionary, sizeof(dictionary));
    output[11U] = (uint8_t)(0x50U | (sizeof(key) - 1U));
    memcpy(output + 12U, key, sizeof(key) - 1U);
    output[16U] = (uint8_t)(0x50U | (sizeof(command) - 1U));
    memcpy(output + 17U, command, sizeof(command) - 1U);
    memcpy(output + 30U, offsets, sizeof(offsets));

    uint8_t *trailer = output + 33U;
    memset(trailer, 0, FORCE_KEYFRAME_TRAILER_BYTES);
    trailer[6U] = 1U; /* offset integer width */
    trailer[7U] = 1U; /* object reference width */
    trailer[15U] = FORCE_KEYFRAME_OBJECTS;
    trailer[31U] = 30U; /* offset table location */
    *output_len = FORCE_KEYFRAME_PLIST_BYTES;
    return true;
}
