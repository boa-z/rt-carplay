/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Luban-Lite sound0 output. Packet callbacks enqueue into independent bounded
 * jitter buffers. One worker owns all decoders, the mixer and audio device.
 * Compressed payloads must use carplay_audio_sink_packet(), with the format
 * negotiated by RTSP. The PCM function remains a compatibility entry. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Creates the per-stream queues, mixer, wake semaphore and writer thread.  The audio
 * device playback starts with the first decoded packet.  Returns 0 on
 * success, a negative RTCP error otherwise. */
int carplay_audio_sink_start(void);

/* Stops the writer thread, stops the replay stream and closes the device. */
void carplay_audio_sink_stop(void);

bool carplay_audio_sink_is_running(void);

/* Encoded packets waiting for the audio worker (all streams). */
size_t carplay_audio_sink_pending(void);

/* Compatibility: enqueue local little-endian PCM on the main lane. */
int carplay_audio_sink_pcm(uint32_t sample_rate, uint8_t channels,
                           const uint8_t *data, size_t len);

/* Counters since start; any output pointer may be NULL.  dropped_bytes counts
 * encoded or PCM bytes discarded by bounded queues. */
void carplay_audio_sink_stats(unsigned *packets, unsigned *dropped_bytes,
                              unsigned *write_errors, unsigned *replays);

/* Queues the sender's SET_PARAMETER volume (dB; 0 = full scale, the AirPlay
 * range is [-144, 0]).  The writer thread applies it through the software master gain;
 * a value that arrives before playback starts is kept until then. */
void carplay_audio_sink_set_volume_db(float volume_db);

/* Last requested volume in normalized percent, or -1 when unset. */
int carplay_audio_sink_volume_percent(void);

#include "audio_codec.h"
int carplay_audio_sink_packet(uint16_t type, const rtcp_audio_format_t *, const uint8_t *, size_t, uint32_t timestamp, uint64_t arrival_us);
void carplay_audio_sink_stream_stop(uint16_t type);

#include "airplay_audio.h"
void carplay_audio_sink_modes(const airplay_audio_modes_t *);
bool carplay_audio_sink_duck(bool ducked, float volume_db, uint32_t duration_ms);
/* Local HMI/critical audio never shares a remote stream queue. */
int carplay_audio_sink_local_pcm(const uint8_t *, size_t, uint32_t rate, uint8_t channels, bool critical);

void carplay_audio_sink_slot_stop(unsigned slot);
