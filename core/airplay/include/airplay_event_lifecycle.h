/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>

/* A live listener can be reused. After a used key has lost its listener, a
 * replacement would restart AEAD counters with the same Pair-Verify key. */
static inline bool airplay_event_lifecycle_may_start(bool listener_active,
                                                     bool event_key_used,
                                                     bool event_failed)
{
    return !event_failed && (listener_active || !event_key_used);
}

static inline bool airplay_event_lifecycle_is_unexpected_exit(
    bool connected,
    bool intentionally_stopping,
    bool server_stopping)
{
    return !connected && !intentionally_stopping && !server_stopping;
}
