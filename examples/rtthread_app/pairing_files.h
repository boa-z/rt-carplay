/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
int carplay_file_load(const char *path, uint8_t *data, size_t capacity, size_t *length);
int carplay_file_save(const char *path, const uint8_t *data, size_t length);
