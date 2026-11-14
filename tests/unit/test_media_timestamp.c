/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "media_timestamp.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    media_timestamp_mapper_t mapper;
    bool sender_clock = false;
    media_timestamp_mapper_reset(&mapper);

    assert(media_timestamp_mapper_map(&mapper, UINT64_C(10000000000), 1000U,
                                      &sender_clock) == 0U);
    assert(sender_clock);
    assert(media_timestamp_mapper_map(&mapper, UINT64_C(10016666667), 9000000U,
                                      &sender_clock) == 1500U);
    assert(sender_clock);
    assert(media_timestamp_mapper_map(&mapper, UINT64_C(10056666667), 60000000U,
                                      &sender_clock) == 5100U);
    assert(sender_clock);

    /* A sender clock reset switches to arrival time and preserves the last PTS. */
    assert(media_timestamp_mapper_map(&mapper, UINT64_C(9000000000), 70000U,
                                      &sender_clock) == 5100U);
    assert(!sender_clock);
    assert(media_timestamp_mapper_map(&mapper, UINT64_C(10000000000), 90000U,
                                      &sender_clock) == 6900U);
    assert(!sender_clock);

    media_timestamp_mapper_reset(&mapper);
    assert(media_timestamp_mapper_map(&mapper, 0U, 5000U,
                                      &sender_clock) == 0U);
    assert(!sender_clock);
    assert(media_timestamp_mapper_map(&mapper, 0U, 6000U,
                                      &sender_clock) == 90U);
    assert(!sender_clock);

    media_timestamp_mapper_reset(&mapper);
    assert(media_timestamp_mapper_map(&mapper, UINT64_C(1000000000), 0U,
                                      &sender_clock) == 0U);
    const uint64_t sender_after_48000_seconds =
        UINT64_C(1000000000) + UINT64_C(48000) * UINT64_C(1000000000);
    assert(media_timestamp_mapper_map(&mapper, sender_after_48000_seconds, 0U,
                                      &sender_clock) == UINT32_C(25032704));
    assert(sender_clock);

    puts("media timestamp host test passed");
    return 0;
}
