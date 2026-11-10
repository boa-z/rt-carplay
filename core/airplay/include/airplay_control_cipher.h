/* SPDX-License-Identifier: GPL-3.0-or-later; ControlCipher framing follows DiPlay */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_CONTROL_CIPHER_KEY_BYTES 32U
#define AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES 4096U
#define AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES 18U
/* Apple's C control transport seals at most kMaxMessageWriteSize = 1024
 * plaintext bytes per frame; longer RTSP messages are split across frames and
 * the peer reassembles the byte stream. */
#define AIRPLAY_CONTROL_CIPHER_MESSAGE_CHUNK_BYTES 1024U

typedef struct airplay_control_cipher airplay_control_cipher_t;

typedef enum {
    AIRPLAY_CONTROL_CIPHER_OK = 0,
    AIRPLAY_CONTROL_CIPHER_INCOMPLETE,
    AIRPLAY_CONTROL_CIPHER_OUTPUT_TOO_SMALL,
    AIRPLAY_CONTROL_CIPHER_INVALID_ARGUMENT,
    AIRPLAY_CONTROL_CIPHER_FRAME_TOO_LARGE,
    AIRPLAY_CONTROL_CIPHER_AUTH_FAILED,
    AIRPLAY_CONTROL_CIPHER_CRYPTO_FAILED,
    AIRPLAY_CONTROL_CIPHER_FAILED_STATE,
} airplay_control_cipher_result_t;

/* read_key/write_key are directional event-channel keys from Pair-Verify. */
airplay_control_cipher_result_t airplay_control_cipher_create(
    const uint8_t read_key[AIRPLAY_CONTROL_CIPHER_KEY_BYTES],
    const uint8_t write_key[AIRPLAY_CONTROL_CIPHER_KEY_BYTES],
    airplay_control_cipher_t **out_cipher);

/* Encodes one bounded [u16le length][ciphertext][tag] frame. */
airplay_control_cipher_result_t airplay_control_cipher_encrypt(
    airplay_control_cipher_t *cipher,
    const uint8_t *plaintext,
    size_t plaintext_len,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len);

/* Encodes a whole RTSP message as one or more frames of at most
 * AIRPLAY_CONTROL_CIPHER_MESSAGE_CHUNK_BYTES plaintext bytes each, mirroring
 * Apple's C transport (`kMaxMessageWriteSize`).  Consecutive frames advance the
 * write counter, and the peer concatenates their plaintext.  `output_capacity`
 * must hold plaintext_len plus one overhead per chunk. */
airplay_control_cipher_result_t airplay_control_cipher_encrypt_message(
    airplay_control_cipher_t *cipher,
    const uint8_t *plaintext,
    size_t plaintext_len,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len);

/* Diagnostic A/B for the wireless bring-up: encode the whole message as ONE
 * frame instead of the 1024-byte chunking above.  The message must fit
 * AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES; larger messages return
 * AIRPLAY_CONTROL_CIPHER_FRAME_TOO_LARGE.  Interoperability with a real
 * controller is unverified; the default path stays chunked. */
airplay_control_cipher_result_t airplay_control_cipher_encrypt_message_single(
    airplay_control_cipher_t *cipher,
    const uint8_t *plaintext,
    size_t plaintext_len,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len);

/* Incrementally consumes encrypted bytes. `input_consumed` reports exactly
 * how much of this chunk was accepted. Complete plaintext frames are appended
 * to output; partial ciphertext remains in the cipher until the next call.
 * After OUTPUT_TOO_SMALL, call again (possibly with no input) and a larger
 * output buffer. Authentication/format failure poisons this cipher session. */
airplay_control_cipher_result_t airplay_control_cipher_decrypt(
    airplay_control_cipher_t *cipher,
    const uint8_t *input,
    size_t input_len,
    size_t *input_consumed,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len);

void airplay_control_cipher_destroy(airplay_control_cipher_t *cipher);
