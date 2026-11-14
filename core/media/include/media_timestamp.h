/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool initialized;
    bool sender_clock_active;
    uint64_t sender_origin_ns;
    uint64_t arrival_origin_us;
    uint64_t pts_origin_ticks;
    uint64_t last_sender_ns;
    uint64_t last_pts_ticks;
} media_timestamp_mapper_t;

void media_timestamp_mapper_reset(media_timestamp_mapper_t *mapper);

/* Convert sender/arrival time to 90 kHz RTP ticks, retaining 32-bit RTP wrap. */
uint32_t media_timestamp_mapper_map(media_timestamp_mapper_t *mapper,
                                    uint64_t sender_time_ns,
                                    uint64_t arrival_time_us,
                                    bool *out_used_sender_clock);
