/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdlib.h>

/*
 * Portable version of the upstream ESP-IDF shim.  The core has no PSRAM
 * concept; a platform that wants a different allocator can replace this
 * helper or route the module through rtcp_os later.
 */
static inline void *airplay_control_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}
