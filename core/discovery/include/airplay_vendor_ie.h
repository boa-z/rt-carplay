/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Apple CarPlay vendor information element advertised in the SoftAP beacons
 * and probe responses:
 *
 *   DD 08 00 A0 40 00 00 02 00 22
 *   ^  ^  ^^^^^^^^^^^ ^^^^^^^^^^^^
 *   |  |  OUI 00:A0:40 Apple CarPlay type/subtype
 *   |  length
 *   element id 221 (vendor specific)
 *
 * These are the bytes the vendor firmware uses; the Realtek driver applies
 * them through SetCusIE.  Keep this module platform-neutral so any Wi-Fi
 * port can hand the same blob to its own vendor-IE hook.
 */
#define AIRPLAY_VENDOR_IE_BYTES 10U

const uint8_t *airplay_vendor_ie(size_t *len);
bool airplay_vendor_ie_valid(const uint8_t *ie, size_t len);
