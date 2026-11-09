/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Luban-Lite audio sink: AirPlay PCM -> RT-Thread "sound0" playback.
 *
 * The AirPlay poll thread delivers decrypted PCM (signed 16-bit little
 * endian, interleaved) from the audio DataStream callback.  This sink copies
 * it into a ring buffer and a dedicated thread feeds the audio device, so a
 * busy network poll never blocks on the replay queue.  The device is
 * configured lazily from the negotiated stream format on the first packet and
 * reconfigured if a later SETUP changes it.
 *
 * The sink needs CONFIG_AIC_USING_AUDIO and the "sound0" device registered by
 * the SDK audio driver (see bsp/examples/test-audio/aplay.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Creates the ring buffer, wake semaphore and writer thread.  The audio
 * device is not touched until the first PCM packet arrives.  Returns 0 on
 * success, a negative RTCP error otherwise. */
int carplay_audio_sink_start(void);

/* Stops the writer thread, stops the replay stream and closes the device. */
void carplay_audio_sink_stop(void);

bool carplay_audio_sink_is_running(void);

/* Bytes waiting for the audio device. */
size_t carplay_audio_sink_pending(void);

/* Enqueue one decrypted PCM packet.  Called from the AirPlay poll thread and
 * never blocks on the device; when the ring is full the packet is dropped
 * whole (counted).  sample_rate/channels describe the PCM layout. */
int carplay_audio_sink_pcm(uint32_t sample_rate, uint8_t channels,
                           const uint8_t *data, size_t len);

/* Counters since start; any output pointer may be NULL.  dropped_bytes counts
 * PCM discarded because the ring was full. */
void carplay_audio_sink_stats(unsigned *packets, unsigned *dropped_bytes,
                              unsigned *write_errors, unsigned *replays);

/* Queues the sender's SET_PARAMETER volume (dB; 0 = full scale, the AirPlay
 * range is [-144, 0]).  The writer thread applies it through the codec mixer;
 * a value that arrives before playback starts is kept until then.  The scale
 * is mapped linearly over [-45, 0] dB to the codec's 0..100 range. */
void carplay_audio_sink_set_volume_db(float volume_db);

/* Last requested volume in codec percent, or -1 when the sender never set
 * one (the codec keeps its default). */
int carplay_audio_sink_volume_percent(void);
