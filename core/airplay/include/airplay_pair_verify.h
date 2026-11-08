/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Accessory-side AirPlay Pair-Verify state machine. The Ed25519 identity and
 * paired-controller store are supplied by the platform; no key is embedded.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_PAIR_VERIFY_KEY_BYTES 32U
#define AIRPLAY_PAIR_VERIFY_SIGNATURE_BYTES 64U
#define AIRPLAY_PAIR_VERIFY_MAX_IDENTIFIER_BYTES 64U
#define AIRPLAY_PAIR_VERIFY_MAX_MESSAGE_BYTES 4096U

typedef struct airplay_pair_verify airplay_pair_verify_t;

typedef bool (*airplay_pair_verify_random_cb_t)(uint8_t *output,
                                                size_t output_len,
                                                void *context);
typedef bool (*airplay_pair_verify_sign_cb_t)(const uint8_t *message,
                                              size_t message_len,
                                              uint8_t signature[AIRPLAY_PAIR_VERIFY_SIGNATURE_BYTES],
                                              void *context);
typedef bool (*airplay_pair_verify_check_signature_cb_t)(
    const uint8_t public_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES],
    const uint8_t *message,
    size_t message_len,
    const uint8_t signature[AIRPLAY_PAIR_VERIFY_SIGNATURE_BYTES],
    void *context);
typedef bool (*airplay_pair_verify_lookup_controller_cb_t)(
    const uint8_t *identifier,
    size_t identifier_len,
    uint8_t public_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES],
    void *context);

typedef struct {
    const uint8_t *accessory_identifier;
    size_t accessory_identifier_len;
    airplay_pair_verify_random_cb_t random;
    airplay_pair_verify_sign_cb_t sign_accessory;
    airplay_pair_verify_check_signature_cb_t verify_controller;
    airplay_pair_verify_lookup_controller_cb_t lookup_controller;
    void *provider_context;
} airplay_pair_verify_config_t;

typedef enum {
    AIRPLAY_PAIR_VERIFY_OK = 0,
    AIRPLAY_PAIR_VERIFY_INVALID_ARGUMENT,
    AIRPLAY_PAIR_VERIFY_NO_MEMORY,
    AIRPLAY_PAIR_VERIFY_RANDOM_FAILED,
    AIRPLAY_PAIR_VERIFY_CRYPTO_FAILED,
    AIRPLAY_PAIR_VERIFY_AUTH_FAILED,
    AIRPLAY_PAIR_VERIFY_STATE_ERROR,
    AIRPLAY_PAIR_VERIFY_BUFFER_TOO_SMALL,
} airplay_pair_verify_result_t;

typedef struct {
    uint8_t read_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
    uint8_t write_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
} airplay_pair_verify_control_keys_t;

/* Event-channel keys use a separate HKDF label set from RTSP ControlCipher. */
typedef struct {
    uint8_t read_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
    uint8_t write_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES];
} airplay_pair_verify_event_keys_t;

airplay_pair_verify_result_t airplay_pair_verify_create(
    const airplay_pair_verify_config_t *config,
    airplay_pair_verify_t **out_session);

/* Handles one application/pairing+tlv8 body and writes its TLV8 response.
 * Invalid protocol/authentication messages return an AirPlay error TLV where
 * possible. The caller must use a fresh session after any failed exchange. */
airplay_pair_verify_result_t airplay_pair_verify_handle(
    airplay_pair_verify_t *session,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len);

/* Control keys are available only after a verified state-3 message. */
airplay_pair_verify_result_t airplay_pair_verify_get_control_keys(
    const airplay_pair_verify_t *session,
    airplay_pair_verify_control_keys_t *out_keys);

/* Event-channel keys are available only after a verified state-3 message. */
airplay_pair_verify_result_t airplay_pair_verify_get_event_keys(
    const airplay_pair_verify_t *session,
    airplay_pair_verify_event_keys_t *out_keys);

/* Derives the Screen DataStream output key without exposing the Pair-Verify
 * shared secret. The shared secret remains private until session destroy so
 * SETUP can supply its streamConnectionID after Pair-Verify completes. */
airplay_pair_verify_result_t airplay_pair_verify_derive_screen_output_key(
    const airplay_pair_verify_t *session,
    uint64_t stream_connection_id,
    uint8_t output_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES]);

void airplay_pair_verify_destroy(airplay_pair_verify_t *session);
