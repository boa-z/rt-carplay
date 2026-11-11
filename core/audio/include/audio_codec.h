/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { RTCP_AUDIO_LPCM, RTCP_AUDIO_AAC_LC, RTCP_AUDIO_OPUS } rtcp_audio_codec_kind_t;
typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    rtcp_audio_codec_kind_t codec;
    uint32_t clock_rate;
    uint8_t stream_slot;
    bool stream_slot_valid;
} rtcp_audio_format_t;
typedef struct rtcp_audio_decoder rtcp_audio_decoder_t;
/* Exactly one negotiated bit. Availability reflects compiled decoders. */
bool rtcp_audio_format_parse(uint64_t bits, rtcp_audio_format_t *format);
uint64_t rtcp_audio_output_formats(void);
int rtcp_audio_decoder_create(const rtcp_audio_format_t *, rtcp_audio_decoder_t **);
/* Input is an AAC raw access unit, Opus packet, or LITTLE-endian PCM.
 * Output is host-endian signed 16 bit PCM; capacity/count are total samples.
 * A decoder belongs to one stream and one audio worker. */
int rtcp_audio_decoder_decode(rtcp_audio_decoder_t *, const uint8_t *, size_t,
                              int16_t *, size_t capacity, size_t *samples);
void rtcp_audio_decoder_destroy(rtcp_audio_decoder_t *);
