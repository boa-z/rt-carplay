#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INPUT_BRIDGE_HID_UID_TOUCH 0x2a2a2a2aU
#define INPUT_BRIDGE_TOUCH_CONTACTS 2U
#define INPUT_BRIDGE_HID_REPORT_BYTES 12U

typedef struct input_bridge input_bridge_t;

typedef enum {
    INPUT_BRIDGE_OK = 0,
    INPUT_BRIDGE_INVALID_ARGUMENT,
    INPUT_BRIDGE_NO_CONTACT_SLOT,
    INPUT_BRIDGE_UNKNOWN_CONTACT,
    INPUT_BRIDGE_HID_NOT_CONNECTED,
} input_bridge_result_t;

typedef enum {
    INPUT_BRIDGE_TOUCH_DOWN,
    INPUT_BRIDGE_TOUCH_MOVE,
    INPUT_BRIDGE_TOUCH_UP,
    INPUT_BRIDGE_TOUCH_CANCEL,
} input_bridge_phase_t;

typedef bool (*input_bridge_report_cb_t)(uint32_t hid_uid,
                                         const uint8_t *report,
                                         size_t report_len,
                                         void *context);

input_bridge_t *input_bridge_create(uint16_t width,
                                    uint16_t height,
                                    uint16_t rotation_degrees,
                                    input_bridge_report_cb_t report_cb,
                                    void *context);
void input_bridge_destroy(input_bridge_t *bridge);
bool input_bridge_set_display(input_bridge_t *bridge,
                              uint16_t width,
                              uint16_t height,
                              uint16_t rotation_degrees);
input_bridge_result_t input_bridge_touch(input_bridge_t *bridge,
                                         input_bridge_phase_t phase,
                                         uint16_t pointer_id,
                                         double x_normalized,
                                         double y_normalized,
                                         uint64_t now_ms);
void input_bridge_tick(input_bridge_t *bridge, uint64_t now_ms);
void input_bridge_release_all(input_bridge_t *bridge);
size_t input_bridge_active_contacts(const input_bridge_t *bridge);
uint32_t input_bridge_invalid_events(const input_bridge_t *bridge);
bool input_bridge_last_report(const input_bridge_t *bridge,
                              uint8_t report[INPUT_BRIDGE_HID_REPORT_BYTES]);

