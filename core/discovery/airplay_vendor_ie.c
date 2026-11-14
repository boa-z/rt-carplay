/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_vendor_ie.h"

static const uint8_t carplay_vendor_ie[AIRPLAY_VENDOR_IE_BYTES] = {
    0xDD, 0x08, 0x00, 0xA0, 0x40, 0x00, 0x00, 0x02, 0x00, 0x22,
};

const uint8_t *airplay_vendor_ie(size_t *len)
{
    if (len != NULL) *len = sizeof(carplay_vendor_ie);
    return carplay_vendor_ie;
}

bool airplay_vendor_ie_valid(const uint8_t *ie, size_t len)
{
    return ie != NULL && len == sizeof(carplay_vendor_ie) &&
           ie[0] == 0xDDU && ie[1] == 0x08U &&
           ie[2] == 0x00U && ie[3] == 0xA0U && ie[4] == 0x40U;
}
