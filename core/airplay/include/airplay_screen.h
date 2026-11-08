/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AirPlay screen DataStream framing is implemented from the protocol behavior
 * documented by DiPlay's ScreenStream.kt. See COPYING and THIRD_PARTY_NOTICES.md
 * for notices.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airplay_screen_codec.h"
#include "rtcp/rtcp.h"

#define AIRPLAY_SCREEN_KEY_BYTES 32U
#define AIRPLAY_SCREEN_TAG_BYTES 16U
#define AIRPLAY_SCREEN_CONFIG_BYTES 4096U

typedef struct airplay_screen_stream airplay_screen_stream_t;

typedef enum {
    AIRPLAY_SCREEN_MESSAGE_CONFIG_H264,
    AIRPLAY_SCREEN_MESSAGE_FRAME_H264,
} airplay_screen_message_type_t;

typedef int (*airplay_screen_message_cb_t)(airplay_screen_message_type_t type,
                                           uint8_t *annex_b,
                                           size_t annex_b_len,
                                           size_t prefix_capacity,
                                           uint64_t sender_time_ns,
                                           uint64_t arrival_time_us,
                                           void *context);

/* max_body_bytes includes the 16-byte AEAD tag for encrypted video frames. */
airplay_screen_stream_t *airplay_screen_stream_create(const uint8_t output_key[AIRPLAY_SCREEN_KEY_BYTES],
                                                      size_t max_body_bytes,
                                                      airplay_screen_message_cb_t callback,
                                                      void *context);
void airplay_screen_stream_destroy(airplay_screen_stream_t *stream);
int airplay_screen_derive_output_key(const uint8_t *shared_secret,
                                     size_t shared_secret_len,
                                     uint64_t stream_connection_id,
                                     uint8_t output_key[AIRPLAY_SCREEN_KEY_BYTES]);
/* Video bodies are decrypted and converted to Annex-B in place. The caller
 * must provide a writable buffer and report its writable prefix capacity;
 * the callback may borrow the frame buffer until it returns. On authentication
 * failure the ciphertext region may be zeroized by Mbed TLS. */
int airplay_screen_stream_process(airplay_screen_stream_t *stream,
                                  const uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES],
                                  uint8_t *body,
                                  size_t body_len,
                                  size_t body_prefix_capacity,
                                  uint64_t arrival_time_us);
uint64_t airplay_screen_stream_frame_counter(const airplay_screen_stream_t *stream);
uint32_t airplay_screen_stream_auth_failures(const airplay_screen_stream_t *stream);
bool airplay_screen_crypto_self_test(void);
