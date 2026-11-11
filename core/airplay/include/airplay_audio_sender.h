/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "airplay_audio.h"
typedef struct airplay_audio_sender airplay_audio_sender_t;
typedef struct {
    rtcp_audio_format_t format;
    uint8_t input_key[32];
    uint8_t payload_type; /* CarPlay stream type (100), not the codec enum. */
    unsigned frames_per_packet; /* per channel; 0 selects MTU-safe default */
} airplay_audio_sender_config_t;
typedef struct {
    airplay_audio_sender_config_t sender;
    rtcp_addr_t peer;
    const struct rtcp_net *net;
    bool telephony;
} airplay_audio_input_config_t;
int airplay_audio_sender_create(const airplay_audio_sender_config_t *, airplay_audio_sender_t **);
size_t airplay_audio_sender_frames(const airplay_audio_sender_t *);
/* PCM is host-endian interleaved S16. Muting sends encoded silence, preserving
 * RTP clock continuity. One sender belongs to one capture worker. Returns
 * sealed datagram bytes or a negative error. Every seal consumes its nonce,
 * even if the caller subsequently fails to transmit the packet. */
int airplay_audio_sender_encode(airplay_audio_sender_t *, const int16_t *, size_t frames,
                               bool mute, uint8_t *wire, size_t capacity);
void airplay_audio_sender_destroy(airplay_audio_sender_t *);
