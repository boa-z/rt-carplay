/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CarPlay audio DataStream receiver.  The wire format follows the GPL-3.0
 * playport audio stream implementation (AudioStream.kt; see NOTICE): one RTP
 * packet per UDP datagram, laid out as
 *
 *   12-byte RTP header | ciphertext | 16-byte Poly1305 tag | 8-byte nonce
 *
 * The last eight header bytes (timestamp + SSRC) are the AEAD associated data;
 * the nonce is four zero bytes followed by the eight wire bytes interpreted
 * little-endian (the same order the sender used).  Wire LPCM is big-endian L16; callbacks receive
 * little-endian, interleaved int16 samples. Compressed AAC/Opus access units
 * are delivered unchanged to the independently owned audio decoder worker.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtcp/net.h"
#include "rtcp/os.h"
#include "../../audio/include/audio_codec.h"

#define AIRPLAY_AUDIO_KEY_BYTES 32U
#define AIRPLAY_AUDIO_TAG_BYTES 16U
#define AIRPLAY_AUDIO_NONCE_BYTES 8U
#define AIRPLAY_AUDIO_RTP_HEADER_BYTES 12U
#define AIRPLAY_AUDIO_OVERHEAD_BYTES \
    (AIRPLAY_AUDIO_RTP_HEADER_BYTES + AIRPLAY_AUDIO_TAG_BYTES + AIRPLAY_AUDIO_NONCE_BYTES)
#define AIRPLAY_AUDIO_DEFAULT_MAX_DATAGRAM_BYTES 4096U

/* CarPlay SETUP stream types handled by this receiver. */
#define AIRPLAY_AUDIO_STREAM_MAIN 100U
#define AIRPLAY_AUDIO_STREAM_ALT 101U
#define AIRPLAY_AUDIO_STREAM_HIGH 102U
#define AIRPLAY_AUDIO_MAX_STREAMS 8U
/* Stable slots distinguish same-type streams by negotiated audioType. */
int airplay_audio_slot_for(uint16_t stream_type, const char *audio_type);
uint16_t airplay_audio_slot_type(unsigned slot);

typedef struct airplay_audio_stream airplay_audio_stream_t;

typedef struct {
    bool has_call, call_active;
    bool has_speech;
    int speech_mode;
    bool has_main_audio;
    uint32_t main_audio_entity;
    /* modesChanged is a delta. Permanent and current owners are independent;
     * 0=None, 1=Controller (iPhone), 2=Accessory, as in catplay's wire model.
     */
    bool has_main_audio_permanent;
    uint32_t main_audio_permanent_entity;
    bool has_screen;
    uint32_t screen_entity;
    bool has_screen_permanent;
    uint32_t screen_permanent_entity;
} airplay_audio_modes_t;
/* Parsed SETUP metadata. Input target is always the authenticated RTSP peer. */
typedef struct {
    uint64_t connection_id;
    uint64_t format_bits;
    char audio_type[24];
    uint16_t input_port;
    uint16_t frames_per_packet;
    bool input;
} airplay_audio_options_t;
typedef rtcp_audio_format_t airplay_audio_format_t;

/* Delivers decrypted payload; LPCM is little-endian int16, compressed
 * access units are identified by format->codec.  data is borrowed until the
 * callback returns.  rtp_timestamp is the sender's 32-bit RTP timestamp;
 * arrival_us is the local monotonic receive time. */
typedef int (*airplay_audio_pcm_cb_t)(uint16_t stream_type,
                                      const uint8_t *data,
                                      size_t len,
                                      const airplay_audio_format_t *format,
                                      uint32_t rtp_timestamp,
                                      uint64_t arrival_us,
                                      void *context);

typedef struct {
    uint8_t output_key[AIRPLAY_AUDIO_KEY_BYTES];
    uint16_t stream_type; /* AIRPLAY_AUDIO_STREAM_*; echoed to the callback. */
    uint64_t audio_format_bits; /* SETUP audioFormat field. */
    uint8_t stream_slot;
    bool stream_slot_valid;
    /* UDP socket family: 0/RTCP_AF_INET (default) or RTCP_AF_INET6. */
    uint8_t family;
    size_t max_datagram_bytes;  /* 0 selects the default. */
    airplay_audio_pcm_cb_t on_pcm;
    void *context;
} airplay_audio_stream_config_t;

/* Maps exactly one negotiated format to a compiled codec layout. */
bool airplay_audio_format_from_bits(uint64_t bits, airplay_audio_format_t *format);

bool airplay_audio_stream_type_is_supported(uint64_t stream_type);

/* Binds the data and RTCP control UDP ports (ephemeral). */
int airplay_audio_stream_start(const struct rtcp_os *os,
                               const struct rtcp_net *net,
                               const airplay_audio_stream_config_t *config,
                               airplay_audio_stream_t **out_stream);

uint16_t airplay_audio_stream_get_data_port(const airplay_audio_stream_t *stream);
uint16_t airplay_audio_stream_get_control_port(const airplay_audio_stream_t *stream);
int airplay_audio_stream_get_data_fd(const airplay_audio_stream_t *stream);
int airplay_audio_stream_get_control_fd(const airplay_audio_stream_t *stream);
uint16_t airplay_audio_stream_get_stream_type(const airplay_audio_stream_t *stream);
const airplay_audio_format_t *airplay_audio_stream_get_format(const airplay_audio_stream_t *stream);
uint64_t airplay_audio_stream_get_format_bits(const airplay_audio_stream_t *stream);

/* Decrypts and delivers one wire datagram. */
int airplay_audio_stream_process_datagram(airplay_audio_stream_t *stream,
                                          const uint8_t *datagram,
                                          size_t len,
                                          uint64_t arrival_us);

/* Call from the application poll loop.  Each readable flag must reflect
 * poll() on the matching descriptor.  Drains at most one data datagram per
 * call; the control port is read only to keep its queue empty. */
void airplay_audio_stream_poll(airplay_audio_stream_t *stream,
                               bool data_readable,
                               bool control_readable);

uint32_t airplay_audio_stream_datagrams(const airplay_audio_stream_t *stream);
uint32_t airplay_audio_stream_auth_failures(const airplay_audio_stream_t *stream);

void airplay_audio_stream_stop(airplay_audio_stream_t *stream,
                               const struct rtcp_net *net);
