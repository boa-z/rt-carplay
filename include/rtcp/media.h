/* SPDX-License-Identifier: GPL-3.0-or-later */
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

typedef enum
{
    RTCP_AUDIO_LPCM,
    RTCP_AUDIO_AAC_LC,
    RTCP_AUDIO_OPUS
} rtcp_audio_codec_kind_t;
typedef struct
{
    uint32_t sample_rate;
    uint8_t channels;
    rtcp_audio_codec_kind_t codec;
    uint32_t clock_rate;
    uint8_t stream_slot;
    bool stream_slot_valid;
} rtcp_audio_format_t;

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

typedef struct
{
    rtcp_audio_format_t format;
    uint8_t input_key[32];
    uint8_t payload_type;       /* CarPlay stream type (100), not the codec enum. */
    unsigned frames_per_packet; /* per channel; 0 selects MTU-safe default */
} rtcp_audio_sender_config_t;
typedef struct
{
    rtcp_audio_sender_config_t sender;
    rtcp_addr_t peer;
    const struct rtcp_net *net;
    bool telephony;
} rtcp_audio_input_config_t;

typedef enum
{
    RTCP_VIDEO_CONFIG_H264,
    RTCP_VIDEO_FRAME_H264,
} rtcp_video_message_type_t;
/* Annex-B H.264 (configuration or access unit), never decoded pixels.
 * Buffer and writable prefix are borrowed only until return. sender_time_ns
 * is the sender timestamp; arrival_time_us is local monotonic receive time.
 * An asynchronous sink must copy before returning. No UI policy is implied. */
typedef int (*rtcp_video_packet_cb_t)(rtcp_video_message_type_t type,
                                      uint8_t *annex_b, size_t length, size_t prefix_capacity,
                                      uint64_t sender_time_ns, uint64_t arrival_time_us, void *context);
/* Decrypted audio: LPCM is interleaved little-endian S16; AAC/Opus remain
 * encoded packets identified by format->codec. Never relabel them as PCM.
 * All pointers are borrowed until return, including format. */
typedef int (*rtcp_audio_packet_cb_t)(uint16_t stream_type, const uint8_t *data,
                                      size_t length, const rtcp_audio_format_t *format, uint32_t rtp_timestamp,
                                      uint64_t arrival_time_us, void *context);

typedef struct
{
    /* NULL video selects an intentional discard sink; NULL audio refuses
     * audio SETUP, retaining the protocol's existing behavior. */
    rtcp_video_packet_cb_t on_video;
    void *video_context;
    rtcp_audio_packet_cb_t on_audio;
    void *audio_context;
    void (*on_audio_stop)(uint16_t stream_type, void *context);
    void (*on_audio_stop_ex)(unsigned slot, uint16_t stream_type, void *context);
    bool (*on_audio_input_start)(const rtcp_audio_input_config_t *, void *context);
    void (*on_audio_input_stop)(void *context);
    void (*on_audio_modes)(const rtcp_audio_modes_t *, void *context);
    bool (*on_audio_duck)(bool ducked, float volume_db, uint32_t duration_ms, void *context);
    void (*on_volume)(float volume_db, void *context);
    void *volume_context;
    /* Independent experimental Type 111 H.264 sink. NULL deliberately discards
     * only when receiver.experimental_alt_video is explicitly enabled. */
    rtcp_video_packet_cb_t on_alt_video;
    void *alt_video_context;
} rtcp_media_backend_t;
#endif
