/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS 8U
#define AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES 64U
#define AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES 32U

/* Whole-blob persistence backend (NVS, file, flash partition, ...). */
typedef struct {
    /* Must return RTCP_OK and set *len to 0 when the key does not exist. */
    int (*load)(uint8_t *blob, size_t capacity, size_t *len, void *context);
    int (*save)(const uint8_t *blob, size_t len, void *context);
    int (*erase)(void *context); /* optional */
    void *context;
} airplay_pairing_store_backend_t;

/*
 * Loads the controller table.  Stored controller identifiers and public keys
 * are public pairing metadata; accessory/MFi private keys are never stored.
 * The core assumes a single event-loop thread owns the store.
 */
int airplay_pairing_store_init(const airplay_pairing_store_backend_t *backend);
void airplay_pairing_store_deinit(void);

/* Directly usable as airplay_pair_setup_config_t callbacks. */
bool airplay_pairing_store_save_pairing(
    const uint8_t *identifier,
    size_t identifier_len,
    const uint8_t public_key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES],
    void *context);
bool airplay_pairing_store_lookup_controller(
    const uint8_t *identifier,
    size_t identifier_len,
    uint8_t public_key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES],
    void *context);

int airplay_pairing_store_remove(const uint8_t *identifier, size_t identifier_len);
int airplay_pairing_store_clear(void);
int airplay_pairing_store_count(size_t *out_count);
