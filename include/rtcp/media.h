/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file media.h
 * Media sink, microphone and Classic Now Playing contracts. */
#ifndef RTCP_MEDIA_H
#define RTCP_MEDIA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rtcp/net.h"

#define RTCP_AUDIO_STREAM_MAIN 100U
#define RTCP_AUDIO_STREAM_ALT  101U
#define RTCP_AUDIO_STREAM_HIGH 102U
#define RTCP_AUDIO_MAX_STREAMS 8U

/** Payload encoding selected by the audio negotiation. */
typedef enum
{
    RTCP_AUDIO_LPCM,
    RTCP_AUDIO_AAC_LC,
    RTCP_AUDIO_OPUS
} rtcp_audio_codec_kind_t;
/** Audio sample and RTP clock rates in Hz. A valid slot distinguishes
 * concurrent streams with the same negotiated stream type. */
typedef struct
{
    uint32_t sample_rate;
    uint8_t channels;
    rtcp_audio_codec_kind_t codec;
    uint32_t clock_rate;
    uint8_t stream_slot;
    bool stream_slot_valid;
} rtcp_audio_format_t;

/** Resource-owner delta. Apply fields only when their presence flags are set. */
typedef struct
{
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
} rtcp_audio_modes_t;

/** Negotiated microphone packet format and per-stream encryption material. */
typedef struct
{
    rtcp_audio_format_t format;
    uint8_t input_key[32];
    uint8_t payload_type;       /* CarPlay stream type (100), not the codec enum. */
    unsigned frames_per_packet; /* per channel; 0 selects MTU-safe default */
} rtcp_audio_sender_config_t;
/** Microphone endpoint and borrowed network provider for the active stream. */
typedef struct
{
    rtcp_audio_sender_config_t sender;
    rtcp_addr_t peer;
    const struct rtcp_net *net;
    bool telephony;
} rtcp_audio_input_config_t;

/** Distinguish a decoder-configuration payload from an H.264 access unit. */
typedef enum
{
    RTCP_VIDEO_CONFIG_H264,
    RTCP_VIDEO_FRAME_H264,
} rtcp_video_message_type_t;
/** Annex-B H.264 configuration or access unit, delivered on the poll owner.
 * Buffer and writable prefix are borrowed only until return. sender_time_ns
 * is the sender timestamp; arrival_time_us is sampled after the complete
 * Screen message reaches the receiver, in the local monotonic clock domain.
 * An asynchronous sink must copy before returning. Return RTCP_OK on success
 * or a negative error to report sink failure. */
typedef int (*rtcp_video_packet_cb_t)(rtcp_video_message_type_t type,
                                      uint8_t *annex_b, size_t length, size_t prefix_capacity,
                                      uint64_t sender_time_ns, uint64_t arrival_time_us, void *context);
/** Decrypted audio on the poll owner. LPCM is interleaved little-endian S16.
 * AAC/Opus remain encoded. All pointers are borrowed until return. Return
 * RTCP_OK on success or a negative sink error. */
typedef int (*rtcp_audio_packet_cb_t)(uint16_t stream_type, const uint8_t *data,
                                      size_t length, const rtcp_audio_format_t *format, uint32_t rtp_timestamp,
                                      uint64_t arrival_time_us, void *context);

/** Classic Now Playing delta. NULL text fields and absent playback state
 * leave the host's previous values unchanged. Text is UTF-8, bounded to
 * 128 bytes per field and borrowed for the callback only. */
typedef struct
{
    const char *title, *artist, *album;
    bool has_playing, playing;
} rtcp_now_playing_t;

/** Deliver one borrowed metadata delta synchronously on its protocol owner. */
typedef void (*rtcp_now_playing_cb_t)(const rtcp_now_playing_t *, void *context);

/** Classic playback metadata delta. Times are milliseconds. The transfer
 * ID is iAP2 metadata. Artwork may come from an AirPlay command or a
 * completed iAP2 file transfer matching the current Now Playing ID.
 * Text and artwork are borrowed until callback return. An absent field
 * preserves its previous value. Present artwork of length zero clears it. */
typedef struct
{
    rtcp_now_playing_t item;
    bool invalidated;
    bool has_duration, has_elapsed, has_artwork_transfer_id;
    uint32_t duration_ms, elapsed_ms;
    uint8_t artwork_transfer_id;
    bool has_artwork;
    const uint8_t *artwork;
    size_t artwork_length;
} rtcp_now_playing_details_t;

/** Deliver a borrowed delta or invalidation on the protocol owner. Copy
 * text and artwork before returning if retained by the host. */
typedef void (*rtcp_now_playing_details_cb_t)(const rtcp_now_playing_details_t *, void *context);

/** Media endpoints owned by the host and called synchronously by Receiver. */
typedef struct
{
    /** NULL video selects an intentional discard sink; NULL audio refuses
     * audio SETUP, retaining the protocol's existing behavior. */
    rtcp_video_packet_cb_t on_video;
    void *video_context;
    rtcp_audio_packet_cb_t on_audio; /**< Required to accept Audio SETUP. */
    void *audio_context;
    /** Retire resources for the stopped stream type. The slot-aware
     * callback takes precedence when both stop callbacks are registered. */
    void (*on_audio_stop)(uint16_t stream_type, void *context);
    /** Preferred stop notification when the sink distinguishes stream slots. */
    void (*on_audio_stop_ex)(unsigned slot, uint16_t stream_type, void *context);
    /** Borrow the negotiated configuration until return. Return true after
     * starting capture, false if the microphone endpoint could not start. */
    bool (*on_audio_input_start)(const rtcp_audio_input_config_t *, void *context);
    /** Retire capture for the active microphone endpoint. */
    void (*on_audio_input_stop)(void *context);
    /** Borrow the delta only during the callback. Retain absent fields. */
    void (*on_audio_modes)(const rtcp_audio_modes_t *, void *context);
    /** Apply a duck/unduck request. Return true when the host accepts it. */
    bool (*on_audio_duck)(bool ducked, float volume_db, uint32_t duration_ms, void *context);
    /** Handle the phone's requested gain in dB using volume_context. */
    void (*on_volume)(float volume_db, void *context);
    void *volume_context;
    /** Independent experimental Type 111 H.264 sink. NULL deliberately discards
     * only when receiver.experimental_alt_video is explicitly enabled. */
    rtcp_video_packet_cb_t on_alt_video;
    void *alt_video_context;
} rtcp_media_backend_t;
#endif
