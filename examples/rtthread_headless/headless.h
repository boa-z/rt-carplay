/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_EXAMPLE_HEADLESS_H
#define RTCP_EXAMPLE_HEADLESS_H
#include <stdatomic.h>
#include "rtcp/receiver.h"

/* An ordinary-thread example, not an SDK singleton/service API. The host
 * brings its network/filesystem up, supplies real secure entropy in os,
 * and keeps all configuration strings/tables/contexts alive until return.
 * No display, codec, touch device, board header or RTOS-private API is used. */
typedef struct
{
    const struct rtcp_os *os;
    const struct rtcp_net *net;
    const char *name;
    const char *device_id;        /* Stable MAC-format accessory identifier. */
    const char *seed_path;        /* Private 32-byte Ed25519 seed, created if absent. */
    const char *store_path;       /* SDK controller table, distinct from seed_path. */
    uint16_t width, height, port; /* port 0 selects the SDK default. */
    bool advertise;               /* Requires an IPv4 address and host_name. */
    const char *host_name;
    uint8_t ipv4[4];
    rtcp_receiver_event_cb_t on_state;
    /* Owner-thread callback; public_key is borrowed only until return. */
    void (*on_ready)(uint16_t port, const uint8_t public_key[32], void *context);
    void *context;
} rtcp_headless_config_t;

typedef struct
{
    unsigned video_configs, video_frames;
    uint64_t video_bytes;
    rtcp_receiver_diagnostics_t diagnostics;
} rtcp_headless_result_t;

/* Blocking. One caller owns every Receiver/store/discovery call. Another
 * thread may only set stop_requested and join/wait for this function to
 * return. Do not reset that flag during the call. result is written only by
 * the owner; inspect after joining. Nested/concurrent example runs return
 * ESTATE because the SDK controller store is process-global. Do not run
 * alongside another application using that same store. No callbacks follow
 * return. Same-directory rename, private filesystem access and exclusive paths are required. */
int rtcp_headless_run(const rtcp_headless_config_t *config,
                      const atomic_bool *stop_requested,
                      rtcp_headless_result_t *result);
#endif
