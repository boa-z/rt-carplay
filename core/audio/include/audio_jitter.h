/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define RTCP_AUDIO_PACKET_MAX 4096U
typedef struct rtcp_audio_jitter rtcp_audio_jitter_t;
typedef struct { uint64_t accepted, late, duplicates, overflow; size_t queued; } rtcp_audio_jitter_stats_t;
/* No internal locks: enqueue/pop/flush must be serialized by the caller.
 * Ordering uses authenticated RTP timestamps (including wrap), not unauthenticated
 * RTP sequence numbers. delay_us is a bounded startup/reordering wait. */
rtcp_audio_jitter_t *rtcp_audio_jitter_create(size_t packets, uint32_t delay_us);
void rtcp_audio_jitter_destroy(rtcp_audio_jitter_t *);
void rtcp_audio_jitter_flush(rtcp_audio_jitter_t *);
int rtcp_audio_jitter_push(rtcp_audio_jitter_t *, const uint8_t *, size_t, uint32_t timestamp, uint64_t arrival_us);
/* Returns bytes, zero when not due, or a negative error. */
int rtcp_audio_jitter_pop(rtcp_audio_jitter_t *, uint8_t *, size_t, uint32_t *timestamp, uint64_t now_us);
void rtcp_audio_jitter_stats(const rtcp_audio_jitter_t *, rtcp_audio_jitter_stats_t *);
