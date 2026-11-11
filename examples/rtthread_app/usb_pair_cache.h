/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "lockdown_pair.h"
int carplay_usb_pair_cache_load(const char *path, rtcp_pair_record_t *record);
int carplay_usb_pair_cache_save(const char *path, const rtcp_pair_record_t *record);
