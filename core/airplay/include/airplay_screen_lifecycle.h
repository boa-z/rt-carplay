/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>

/* A Screen DataStream output key has one TCP peer and one nonce-counter
 * epoch; the listener is closed after the first accept. */
static inline bool airplay_screen_lifecycle_may_accept(bool peer_accepted,
                                                       bool stop_requested)
{
    return !peer_accepted && !stop_requested;
}

static inline bool airplay_screen_lifecycle_is_unexpected_exit(
    bool stop_requested)
{
    return !stop_requested;
}
