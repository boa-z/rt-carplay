/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
/* Blocking operations, one owning worker only. Do not call from LVGL or mix
 * with shell mutation commands. Session close waits for the iAP2 owner. */
int carplay_usb_open(void);
bool carplay_usb_attached(void);
int carplay_usb_pair(void);
int carplay_usb_start_iap2(void);
bool carplay_usb_iap2_running(void);
int carplay_usb_close_session(void);
