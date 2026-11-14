#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "input_bridge.h"

typedef struct {
    uint8_t report[INPUT_BRIDGE_HID_REPORT_BYTES];
    uint32_t uid;
    unsigned sends;
} harness_t;

static bool capture_report(uint32_t uid, const uint8_t *report, size_t len, void *context)
{
    harness_t *h = context;
    assert(len == sizeof(h->report));
    h->uid = uid;
    memcpy(h->report, report, len);
    ++h->sends;
    return true;
}

static uint16_t read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8U);
}

static void verify_rotation(uint16_t rotation,
                            uint16_t expected_x,
                            uint16_t expected_y)
{
    harness_t h = {0};
    input_bridge_t *bridge = input_bridge_create(320, 240, rotation,
                                                  capture_report, &h);
    assert(bridge != NULL);
    assert(input_bridge_touch(bridge, INPUT_BRIDGE_TOUCH_DOWN, 1,
                              0.2, 0.3, 0) == INPUT_BRIDGE_OK);
    assert(read_u16_le(&h.report[2]) == expected_x);
    assert(read_u16_le(&h.report[4]) == expected_y);
    input_bridge_destroy(bridge);
}

int main(void)
{
    harness_t h = {0};
    input_bridge_t *bridge = input_bridge_create(320, 240, 0, capture_report, &h);
    assert(bridge != NULL);

    assert(input_bridge_touch(bridge, INPUT_BRIDGE_TOUCH_DOWN, 10, 0.5, 0.25, 0) == INPUT_BRIDGE_OK);
    assert(h.uid == INPUT_BRIDGE_HID_UID_TOUCH && h.report[0] == 0 && h.report[1] == 1);
    assert(h.report[2] == 160 && h.report[3] == 0);
    assert(h.report[4] == 60 && h.report[5] == 0);
    assert(input_bridge_active_contacts(bridge) == 1);

    assert(input_bridge_touch(bridge, INPUT_BRIDGE_TOUCH_DOWN, 20, 1.0, 1.0, 10) == INPUT_BRIDGE_OK);
    assert(h.report[6] == 1 && h.report[7] == 1);
    assert(h.report[8] == 64 && h.report[9] == 1);
    assert(h.report[10] == 240 && h.report[11] == 0);
    assert(input_bridge_active_contacts(bridge) == 2);
    assert(input_bridge_touch(bridge, INPUT_BRIDGE_TOUCH_DOWN, 30, 0.0, 0.0, 11) == INPUT_BRIDGE_NO_CONTACT_SLOT);

    assert(input_bridge_touch(bridge, INPUT_BRIDGE_TOUCH_MOVE, 10, 0.1, 0.9, 20) == INPUT_BRIDGE_OK);
    assert(h.report[2] == 32 && h.report[4] == 216);
    assert(input_bridge_touch(bridge, INPUT_BRIDGE_TOUCH_UP, 10, 0.1, 0.9, 30) == INPUT_BRIDGE_OK);
    assert(h.report[1] == 0 && input_bridge_active_contacts(bridge) == 1);

    assert(input_bridge_touch(bridge, INPUT_BRIDGE_TOUCH_MOVE, 20, 0.7, 0.8, 40) == INPUT_BRIDGE_OK);
    assert(h.report[1] == 0 && h.report[7] == 1); /* Lifting slot 0 does not move contact 20. */
    assert(h.report[8] == 224 && h.report[10] == 192);

    input_bridge_tick(bridge, 1039);
    assert(input_bridge_active_contacts(bridge) == 1);
    input_bridge_tick(bridge, 1040);
    assert(input_bridge_active_contacts(bridge) == 0);
    assert(h.report[7] == 0);

    assert(input_bridge_touch(bridge, INPUT_BRIDGE_TOUCH_MOVE, 20, 0.3, 0.4, 1020) == INPUT_BRIDGE_UNKNOWN_CONTACT);
    assert(input_bridge_invalid_events(bridge) == 2);

    verify_rotation(0, 64, 72);
    verify_rotation(90, 224, 48);
    verify_rotation(180, 256, 168);
    verify_rotation(270, 96, 192);

    input_bridge_t *dynamic = input_bridge_create(320, 240, 0, capture_report, &h);
    assert(dynamic != NULL);
    assert(input_bridge_touch(dynamic, INPUT_BRIDGE_TOUCH_DOWN, 9, 0.25, 0.75, 0) == INPUT_BRIDGE_OK);
    assert(h.report[2] == 80 && h.report[4] == 180);
    assert(input_bridge_set_display(dynamic, 1920, 1080, 90));
    assert(input_bridge_active_contacts(dynamic) == 0);
    assert(h.report[1] == 0);
    assert(input_bridge_touch(dynamic, INPUT_BRIDGE_TOUCH_MOVE, 9, 0.25, 0.75, 1) ==
           INPUT_BRIDGE_UNKNOWN_CONTACT);
    assert(input_bridge_touch(dynamic, INPUT_BRIDGE_TOUCH_DOWN, 10, 0.25, 0.75, 2) ==
           INPUT_BRIDGE_OK);
    assert(h.report[1] == 1 && h.report[2] == 0xe0 && h.report[3] == 1 &&
           h.report[4] == 0x0e && h.report[5] == 1);
    assert(!input_bridge_set_display(dynamic, 0, 1080, 90));
    assert(input_bridge_active_contacts(dynamic) == 1);
    input_bridge_release_all(dynamic);
    assert(h.report[1] == 0);

    input_bridge_destroy(dynamic);
    input_bridge_destroy(bridge);
    puts("input_bridge host test passed");
    return 0;
}
