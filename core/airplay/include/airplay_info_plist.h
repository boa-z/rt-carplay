/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AirPlay /info response plist, matching the DiPlay/xcertplay provider
 * structure this stack was ported from.  No identity material is embedded:
 * device_id, name and model are supplied by the caller at runtime.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_INFO_PLIST_MAX_BYTES 6144U

/* DiPlay CarPlay feature mask with audio-output bits cleared (no audio sink). */
#define AIRPLAY_INFO_DEFAULT_FEATURES 0x615203a4e2ULL
/* The audio-output capability bits reported by the CarPlay senders. */
#define AIRPLAY_INFO_AUDIO_FEATURES 0x4500a00ULL
#define AIRPLAY_INFO_DEFAULT_FEATURES_AUDIO \
    (AIRPLAY_INFO_DEFAULT_FEATURES | AIRPLAY_INFO_AUDIO_FEATURES)
#define AIRPLAY_INFO_DEFAULT_STATUS_FLAGS 4U
#define AIRPLAY_INFO_DEFAULT_SOURCE_VERSION "950.7.1"
#define AIRPLAY_INFO_DEFAULT_MODEL "ArtInChip"
#define AIRPLAY_INFO_DEFAULT_MANUFACTURER "rt-carplay"

typedef struct {
    const char *device_id;      /* Required: "AA:BB:CC:DD:EE:FF". */
    const char *bluetooth_id;   /* Optional Classic BT address. */
    const char *name;           /* Optional, defaults to "rt-carplay". */
    const char *model;          /* Optional, defaults to AIRPLAY_INFO_DEFAULT_MODEL. */
    const char *manufacturer;   /* Optional, defaults to AIRPLAY_INFO_DEFAULT_MANUFACTURER. */
    const char *source_version; /* Optional, defaults to AIRPLAY_INFO_DEFAULT_SOURCE_VERSION. */
    uint64_t features;          /* 0 uses AIRPLAY_INFO_DEFAULT_FEATURES. */
    uint32_t status_flags;      /* 0 uses AIRPLAY_INFO_DEFAULT_STATUS_FLAGS. */
    uint16_t width_pixels;      /* Required main display width. */
    uint16_t height_pixels;     /* Required main display height. */
    uint16_t width_physical_mm; /* 0 defaults to 200. */
    uint16_t height_physical_mm;/* 0 defaults to 120. */
    uint8_t max_fps;            /* 0 defaults to 10. */
    bool right_hand_drive;
    /* Advertise the PCM audio sink.  Sets the audio feature bits and emits
     * audioFormats/audioLatencies; leave false when no audio device exists,
     * otherwise the sender opens a stream nobody consumes. */
    bool audio_output;
    bool audio_input; /* only with an available microphone uplink */
    bool audio_compressed; /* advertise only compiled AAC/Opus and an encoded-capable sink */
    uint32_t audio_sample_rate; /* 0 defaults to 44100; 48000 selects 48 kHz. */
} airplay_info_plist_config_t;

/*
 * Emits a binary property list ("bplist00") suitable for the GET /info body.
 * Strings may be UTF-8; non-ASCII text is encoded as UTF-16BE.  Returns false
 * on invalid arguments, non-ASCII-free overlong input, or if the output does
 * not fit.
 */
bool airplay_info_plist_build(const airplay_info_plist_config_t *config,
                              uint8_t *output,
                              size_t output_capacity,
                              size_t *output_len);
