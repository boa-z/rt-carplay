/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "media_timestamp.h"

#include <string.h>

#define MEDIA_TIMESTAMP_RTP_HZ UINT64_C(90000)
#define MEDIA_TIMESTAMP_NS_PER_SECOND UINT64_C(1000000000)
#define MEDIA_TIMESTAMP_US_PER_SECOND UINT64_C(1000000)

static uint64_t nanoseconds_to_rtp_ticks(uint64_t elapsed_ns)
{
    uint64_t seconds = elapsed_ns / MEDIA_TIMESTAMP_NS_PER_SECOND;
    uint64_t remainder_ns = elapsed_ns % MEDIA_TIMESTAMP_NS_PER_SECOND;
    return seconds * MEDIA_TIMESTAMP_RTP_HZ +
           (remainder_ns * MEDIA_TIMESTAMP_RTP_HZ) /
               MEDIA_TIMESTAMP_NS_PER_SECOND;
}

static uint64_t microseconds_to_rtp_ticks(uint64_t elapsed_us)
{
    uint64_t seconds = elapsed_us / MEDIA_TIMESTAMP_US_PER_SECOND;
    uint64_t remainder_us = elapsed_us % MEDIA_TIMESTAMP_US_PER_SECOND;
    return seconds * MEDIA_TIMESTAMP_RTP_HZ +
           (remainder_us * MEDIA_TIMESTAMP_RTP_HZ) /
               MEDIA_TIMESTAMP_US_PER_SECOND;
}

void media_timestamp_mapper_reset(media_timestamp_mapper_t *mapper)
{
    if (mapper != NULL) memset(mapper, 0, sizeof(*mapper));
}

uint32_t media_timestamp_mapper_map(media_timestamp_mapper_t *mapper,
                                    uint64_t sender_time_ns,
                                    uint64_t arrival_time_us,
                                    bool *out_used_sender_clock)
{
    if (out_used_sender_clock != NULL) *out_used_sender_clock = false;
    if (mapper == NULL) return 0U;

    if (!mapper->initialized) {
        mapper->initialized = true;
        mapper->sender_clock_active = sender_time_ns != 0U;
        mapper->sender_origin_ns = sender_time_ns;
        mapper->arrival_origin_us = arrival_time_us;
        mapper->pts_origin_ticks = 0U;
        mapper->last_sender_ns = sender_time_ns;
        mapper->last_pts_ticks = 0U;
        if (out_used_sender_clock != NULL) {
            *out_used_sender_clock = mapper->sender_clock_active;
        }
        return 0U;
    }

    if (mapper->sender_clock_active) {
        if (sender_time_ns != 0U && sender_time_ns >= mapper->last_sender_ns) {
            uint64_t elapsed_ns = sender_time_ns - mapper->sender_origin_ns;
            uint64_t ticks = nanoseconds_to_rtp_ticks(elapsed_ns);
            if (ticks < mapper->last_pts_ticks) ticks = mapper->last_pts_ticks;
            mapper->last_sender_ns = sender_time_ns;
            mapper->last_pts_ticks = ticks;
            if (out_used_sender_clock != NULL) *out_used_sender_clock = true;
            return (uint32_t)ticks;
        }

        /* Rebase to arrival time without moving backwards on the RTP timeline. */
        mapper->sender_clock_active = false;
        mapper->arrival_origin_us = arrival_time_us;
        mapper->pts_origin_ticks = mapper->last_pts_ticks;
        return (uint32_t)mapper->last_pts_ticks;
    }

    if (arrival_time_us < mapper->arrival_origin_us) {
        mapper->arrival_origin_us = arrival_time_us;
        mapper->pts_origin_ticks = mapper->last_pts_ticks;
        return (uint32_t)mapper->last_pts_ticks;
    }

    uint64_t ticks = mapper->pts_origin_ticks +
                     microseconds_to_rtp_ticks(arrival_time_us -
                                               mapper->arrival_origin_us);
    if (ticks < mapper->last_pts_ticks) ticks = mapper->last_pts_ticks;
    mapper->last_pts_ticks = ticks;
    return (uint32_t)ticks;
}
