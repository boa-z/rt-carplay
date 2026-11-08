/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Accessory-side AirPlay Pair-Setup state machine. Pairing credentials,
 * Ed25519 identity operations, pairing storage, and randomness are provided
 * by the platform.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_PAIR_SETUP_KEY_BYTES 32U
#define AIRPLAY_PAIR_SETUP_SIGNATURE_BYTES 64U
#define AIRPLAY_PAIR_SETUP_MAX_IDENTIFIER_BYTES 64U
#define AIRPLAY_PAIR_SETUP_MAX_PAIRING_CODE_BYTES 128U
#define AIRPLAY_PAIR_SETUP_MAX_MESSAGE_BYTES 4096U

typedef struct airplay_pair_setup airplay_pair_setup_t;

typedef bool (*airplay_pair_setup_random_cb_t)(uint8_t *output,
                                               size_t output_len,
                                               void *context);
typedef bool (*airplay_pair_setup_sign_cb_t)(const uint8_t *message,
                                             size_t message_len,
                                             uint8_t signature[AIRPLAY_PAIR_SETUP_SIGNATURE_BYTES],
                                             void *context);
typedef bool (*airplay_pair_setup_verify_cb_t)(
    const uint8_t public_key[AIRPLAY_PAIR_SETUP_KEY_BYTES],
    const uint8_t *message,
    size_t message_len,
    const uint8_t signature[AIRPLAY_PAIR_SETUP_SIGNATURE_BYTES],
    void *context);
typedef bool (*airplay_pair_setup_save_pairing_cb_t)(
    const uint8_t *identifier,
    size_t identifier_len,
    const uint8_t public_key[AIRPLAY_PAIR_SETUP_KEY_BYTES],
    void *context);

typedef struct {
    const char *pairing_code;
    const uint8_t *accessory_identifier;
    size_t accessory_identifier_len;
    const uint8_t *accessory_public_key;
    airplay_pair_setup_random_cb_t random;
    airplay_pair_setup_sign_cb_t sign_accessory;
    airplay_pair_setup_verify_cb_t verify_controller;
    airplay_pair_setup_save_pairing_cb_t save_pairing;
    void *provider_context;
} airplay_pair_setup_config_t;

typedef enum {
    AIRPLAY_PAIR_SETUP_OK = 0,
    AIRPLAY_PAIR_SETUP_INVALID_ARGUMENT,
    AIRPLAY_PAIR_SETUP_NO_MEMORY,
    AIRPLAY_PAIR_SETUP_RANDOM_FAILED,
    AIRPLAY_PAIR_SETUP_CRYPTO_FAILED,
    AIRPLAY_PAIR_SETUP_AUTH_FAILED,
    AIRPLAY_PAIR_SETUP_STATE_ERROR,
    AIRPLAY_PAIR_SETUP_STORAGE_FAILED,
    AIRPLAY_PAIR_SETUP_BUFFER_TOO_SMALL,
} airplay_pair_setup_result_t;

airplay_pair_setup_result_t airplay_pair_setup_create(
    const airplay_pair_setup_config_t *config,
    airplay_pair_setup_t **out_session);

/* Handles one application/pairing+tlv8 body. A failed exchange is terminal;
 * create a new session to retry. */
airplay_pair_setup_result_t airplay_pair_setup_handle(
    airplay_pair_setup_t *session,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len);

bool airplay_pair_setup_is_complete(const airplay_pair_setup_t *session);
void airplay_pair_setup_destroy(airplay_pair_setup_t *session);
