/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The report layout follows the two-contact CarPlay HID report in DiPlay's AirPlayHid.kt.
 * See THIRD_PARTY_NOTICES.md and COPYING for attribution and terms.
 */
#include "input_bridge.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_BRIDGE_STALE_TOUCH_MS 1000U

typedef struct {
    uint16_t pointer_id;
    uint16_t x;
    uint16_t y;
    uint64_t updated_ms;
    bool assigned;
    bool down;
} input_contact_t;

struct input_bridge {
    uint16_t width;
    uint16_t height;
    uint16_t rotation_degrees;
    input_bridge_report_cb_t report_cb;
    void *context;
    input_contact_t contacts[INPUT_BRIDGE_TOUCH_CONTACTS];
    uint8_t last_report[INPUT_BRIDGE_HID_REPORT_BYTES];
    bool has_report;
    uint32_t invalid_events;
};

static uint16_t map_axis(double normalized, uint16_t extent)
{
    double scaled = normalized * (double)extent;
    return (uint16_t)floor(scaled + 0.5);
}

static void map_coordinates(const input_bridge_t *bridge, double x, double y,
                            uint16_t *out_x, uint16_t *out_y)
{
    double mapped_x = x;
    double mapped_y = y;
    switch (bridge->rotation_degrees) {
    case 90:
        mapped_x = 1.0 - y;
        mapped_y = x;
        break;
    case 180:
        mapped_x = 1.0 - x;
        mapped_y = 1.0 - y;
        break;
    case 270:
        mapped_x = y;
        mapped_y = 1.0 - x;
        break;
    default:
        break;
    }
    *out_x = map_axis(mapped_x, bridge->width);
    *out_y = map_axis(mapped_y, bridge->height);
}

static void encode_report(const input_bridge_t *bridge,
                          uint8_t report[INPUT_BRIDGE_HID_REPORT_BYTES])
{
    memset(report, 0, INPUT_BRIDGE_HID_REPORT_BYTES);
    for (size_t slot = 0; slot < INPUT_BRIDGE_TOUCH_CONTACTS; ++slot) {
        const input_contact_t *contact = &bridge->contacts[slot];
        size_t offset = slot * 6U;
        report[offset] = (uint8_t)slot;
        report[offset + 1U] = contact->down ? 0x01U : 0x00U;
        report[offset + 2U] = (uint8_t)(contact->x & 0xffU);
        report[offset + 3U] = (uint8_t)(contact->x >> 8);
        report[offset + 4U] = (uint8_t)(contact->y & 0xffU);
        report[offset + 5U] = (uint8_t)(contact->y >> 8);
    }
}

static bool emit_report(input_bridge_t *bridge)
{
    encode_report(bridge, bridge->last_report);
    bridge->has_report = true;
    if (bridge->report_cb == NULL) {
        return false;
    }
    return bridge->report_cb(INPUT_BRIDGE_HID_UID_TOUCH, bridge->last_report,
                             sizeof(bridge->last_report), bridge->context);
}

static int find_contact(const input_bridge_t *bridge, uint16_t pointer_id)
{
    for (size_t i = 0; i < INPUT_BRIDGE_TOUCH_CONTACTS; ++i) {
        if (bridge->contacts[i].assigned && bridge->contacts[i].pointer_id == pointer_id) {
            return (int)i;
        }
    }
    return -1;
}

input_bridge_t *input_bridge_create(uint16_t width,
                                    uint16_t height,
                                    uint16_t rotation_degrees,
                                    input_bridge_report_cb_t report_cb,
                                    void *context)
{
    if (width == 0 || height == 0 ||
        (rotation_degrees != 0 && rotation_degrees != 90 &&
         rotation_degrees != 180 && rotation_degrees != 270)) {
        return NULL;
    }
    input_bridge_t *bridge = calloc(1, sizeof(*bridge));
    if (bridge == NULL) {
        return NULL;
    }
    bridge->width = width;
    bridge->height = height;
    bridge->rotation_degrees = rotation_degrees;
    bridge->report_cb = report_cb;
    bridge->context = context;
    for (size_t i = 0; i < INPUT_BRIDGE_TOUCH_CONTACTS; ++i) {
        bridge->contacts[i].x = 0;
        bridge->contacts[i].y = 0;
    }
    return bridge;
}

void input_bridge_destroy(input_bridge_t *bridge)
{
    free(bridge);
}

bool input_bridge_set_display(input_bridge_t *bridge,
                              uint16_t width,
                              uint16_t height,
                              uint16_t rotation_degrees)
{
    if (bridge == NULL || width == 0 || height == 0 ||
        (rotation_degrees != 0 && rotation_degrees != 90 &&
         rotation_degrees != 180 && rotation_degrees != 270)) {
        return false;
    }
    if (bridge->width != width || bridge->height != height ||
        bridge->rotation_degrees != rotation_degrees) {
        input_bridge_release_all(bridge);
    }
    bridge->width = width;
    bridge->height = height;
    bridge->rotation_degrees = rotation_degrees;
    return true;
}

input_bridge_result_t input_bridge_touch(input_bridge_t *bridge,
                                         input_bridge_phase_t phase,
                                         uint16_t pointer_id,
                                         double x_normalized,
                                         double y_normalized,
                                         uint64_t now_ms)
{
    if (bridge == NULL || !isfinite(x_normalized) || !isfinite(y_normalized) ||
        x_normalized < 0.0 || x_normalized > 1.0 ||
        y_normalized < 0.0 || y_normalized > 1.0 ||
        phase < INPUT_BRIDGE_TOUCH_DOWN || phase > INPUT_BRIDGE_TOUCH_CANCEL) {
        if (bridge != NULL) {
            ++bridge->invalid_events;
        }
        return INPUT_BRIDGE_INVALID_ARGUMENT;
    }

    int slot = find_contact(bridge, pointer_id);
    if (phase == INPUT_BRIDGE_TOUCH_DOWN) {
        if (slot < 0) {
            for (size_t i = 0; i < INPUT_BRIDGE_TOUCH_CONTACTS; ++i) {
                if (!bridge->contacts[i].down) {
                    slot = (int)i;
                    break;
                }
            }
        }
        if (slot < 0) {
            ++bridge->invalid_events;
            return INPUT_BRIDGE_NO_CONTACT_SLOT;
        }
        bridge->contacts[slot].assigned = true;
        bridge->contacts[slot].pointer_id = pointer_id;
        bridge->contacts[slot].down = true;
    } else if (slot < 0 || !bridge->contacts[slot].down) {
        ++bridge->invalid_events;
        return INPUT_BRIDGE_UNKNOWN_CONTACT;
    }

    input_contact_t *contact = &bridge->contacts[slot];
    map_coordinates(bridge, x_normalized, y_normalized, &contact->x, &contact->y);
    contact->updated_ms = now_ms;
    if (phase == INPUT_BRIDGE_TOUCH_UP || phase == INPUT_BRIDGE_TOUCH_CANCEL) {
        contact->down = false;
        contact->assigned = false;
    }
    return emit_report(bridge) ? INPUT_BRIDGE_OK : INPUT_BRIDGE_HID_NOT_CONNECTED;
}

void input_bridge_release_all(input_bridge_t *bridge)
{
    if (bridge == NULL) {
        return;
    }
    bool had_down_contact = false;
    for (size_t i = 0; i < INPUT_BRIDGE_TOUCH_CONTACTS; ++i) {
        had_down_contact |= bridge->contacts[i].down;
        bridge->contacts[i].down = false;
        bridge->contacts[i].assigned = false;
    }
    if (had_down_contact) {
        (void)emit_report(bridge);
    }
}

void input_bridge_tick(input_bridge_t *bridge, uint64_t now_ms)
{
    if (bridge == NULL) {
        return;
    }
    bool stale = false;
    for (size_t i = 0; i < INPUT_BRIDGE_TOUCH_CONTACTS; ++i) {
        input_contact_t *contact = &bridge->contacts[i];
        if (contact->down && now_ms >= contact->updated_ms &&
            now_ms - contact->updated_ms >= INPUT_BRIDGE_STALE_TOUCH_MS) {
            contact->down = false;
            contact->assigned = false;
            stale = true;
        }
    }
    if (stale) {
        (void)emit_report(bridge);
    }
}

size_t input_bridge_active_contacts(const input_bridge_t *bridge)
{
    if (bridge == NULL) {
        return 0;
    }
    size_t active = 0;
    for (size_t i = 0; i < INPUT_BRIDGE_TOUCH_CONTACTS; ++i) {
        active += bridge->contacts[i].down ? 1U : 0U;
    }
    return active;
}

uint32_t input_bridge_invalid_events(const input_bridge_t *bridge)
{
    return bridge != NULL ? bridge->invalid_events : 0;
}

bool input_bridge_last_report(const input_bridge_t *bridge,
                              uint8_t report[INPUT_BRIDGE_HID_REPORT_BYTES])
{
    if (bridge == NULL || report == NULL || !bridge->has_report) {
        return false;
    }
    memcpy(report, bridge->last_report, INPUT_BRIDGE_HID_REPORT_BYTES);
    return true;
}
