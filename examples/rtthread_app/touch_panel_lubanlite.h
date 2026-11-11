/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Luban-Lite touch panel: RT-Thread CTP device -> CarPlay touch events.
 *
 * The panel driver (GT911 on demo128-nand-carplay) delivers points on its
 * interrupt; this module reads them and forwards DOWN/MOVE/UP with the
 * panel's track id to the application.  Coordinates are scaled from the
 * panel range reported by the driver to the display size passed to start().
 *
 * The CarPlay HID report carries two contacts, so at most two simultaneous
 * panel contacts reach the controller.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* phase is one of RTCP_TOUCH_DOWN / RTCP_TOUCH_MOVE / RTCP_TOUCH_UP;
 * pointer_id is the panel track id. */
typedef void (*carplay_touch_panel_cb_t)(uint8_t phase, uint16_t pointer_id,
                                         uint16_t x, uint16_t y, void *context);

/* Opens the RT-Thread touch device named by CONFIG_AIC_TOUCH_PANEL_NAME
 * (fallback "gt911") and starts the interrupt-driven reader.  Returns 0 on
 * success, RTCP_ENOENT when the device is not registered and another
 * negative RTCP error otherwise. */
int carplay_touch_panel_start(uint16_t width, uint16_t height,
                              carplay_touch_panel_cb_t callback,
                              void *context);

/* Disables the panel interrupt, stops the reader and closes the device. */
void carplay_touch_panel_stop(void);

bool carplay_touch_panel_is_running(void);

/* Points the panel reports, 0 when the reader is not running. */
uint16_t carplay_touch_panel_points(void);

/* Input reads and valid DOWN/MOVE/UP events since the last start. */
void carplay_touch_panel_stats(unsigned *reads, unsigned *events);
