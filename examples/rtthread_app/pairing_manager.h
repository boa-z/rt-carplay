/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#define CARPLAY_PAIR_FILES_MAX 12
#define CARPLAY_PAIR_FILENAME_MAX 96
typedef struct {
    char name[CARPLAY_PAIR_FILENAME_MAX];
    size_t bytes;
} carplay_pair_file_t;
bool carplay_pair_filename_allowed(const char *name);
/* Includes backup-only and temporary-only records; never reads private key contents. */
int carplay_pair_files_scan(const char *directory, carplay_pair_file_t *files,
                            unsigned capacity, unsigned *count, bool *more);
/* Deletes the primary, backup and temporary copy. Caller must stop owners. */
int carplay_pair_file_forget(const char *directory, const char *name);
