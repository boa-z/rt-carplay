/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_TXT_MAX_RECORDS 16U
#define AIRPLAY_TXT_MAX_RECORD_BYTES 255U

typedef struct {
    const char *key;
    const char *value;
} airplay_txt_record_t;

typedef struct {
    const char *device_id;        /* "AA:BB:CC:DD:EE:FF" */
    const char *model;            /* e.g. "AudioAccessory5,1" */
    const char *source_version;   /* e.g. "950.7.1" */
    const char *protocol_version; /* e.g. "1.1" */
    const char *pairing_id;       /* "pi": stable UUID */
    const char *public_key;       /* "pk": hex-encoded Ed25519 public key */
    const char *features;         /* NULL uses AIRPLAY_TXT_DEFAULT_FEATURES */
    const char *flags;            /* NULL uses AIRPLAY_TXT_DEFAULT_FLAGS */
} airplay_txt_config_t;

#define AIRPLAY_TXT_DEFAULT_FEATURES "0x5203a4e2,0x61"
#define AIRPLAY_TXT_DEFAULT_FLAGS "0x4"

/*
 * Fills the Bonjour TXT record set advertised by the _airplay._tcp service.
 * Returns the number of records written, or 0 on invalid input / capacity.
 */
size_t airplay_txt_build(const airplay_txt_config_t *config,
                         airplay_txt_record_t *records,
                         size_t capacity);

/*
 * Encodes the records in DNS-SD TXT wire format: each entry is a one-byte
 * length followed by "key=value".  Returns bytes written, or 0 on overflow.
 */
size_t airplay_txt_encode(const airplay_txt_record_t *records,
                          size_t record_count,
                          uint8_t *output,
                          size_t capacity);
