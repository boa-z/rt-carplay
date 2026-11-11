/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
int carplay_file_load(const char *path, uint8_t *data, size_t capacity, size_t *length);
int carplay_file_save(const char *path, const uint8_t *data, size_t length);

/* This SDK's DFS stores negative errno directly; host/newlib paths may use
 * positive errno. Match a specific error without treating other I/O failures
 * as missing files. Do not abs() an arbitrary errno (INT_MIN can overflow). */
static inline bool carplay_file_errno_is(int observed, int expected)
{
    return observed == expected || observed == -expected;
}
