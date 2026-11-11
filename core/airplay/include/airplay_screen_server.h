/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airplay_screen.h"
#include "rtcp/net.h"
#include "rtcp/os.h"

/*
 * One-peer TCP Screen DataStream listener.  The RTSP server polls get_fd()
 * and calls poll(); 128-byte headers and their bodies are assembled here and
 * passed through airplay_screen_stream_process().
 *
 * One output key is scoped to one accepted peer and one nonce epoch: the
 * listener is closed after accept() and the server never re-listens.
 */
typedef struct airplay_screen_server airplay_screen_server_t;

/* Monotonic elapsed wall time includes callback execution and preemption. */
typedef struct {
    uint32_t recv_calls;
    uint32_t messages;
    uint32_t frames;
    uint64_t wire_bytes;
    uint64_t process_us;
    uint32_t process_max_us;
    uint64_t assembly_us; /* first received header bytes -> complete body */
    uint32_t assembly_max_us;
    uint32_t frame_gap_count;
    uint64_t frame_gap_us;
    uint32_t frame_gap_max_us;
    uint32_t frame_gap_le40ms, frame_gap_le100ms, frame_gap_gt100ms;
    uint32_t sender_gap_count;
    uint64_t sender_gap_us;
    uint32_t sender_gap_max_us;
} airplay_screen_server_stats_t;

typedef void (*airplay_screen_server_exit_cb_t)(void *context);

typedef struct {
    uint16_t requested_port; /* 0 selects an ephemeral port. */
    /* Listener address family: 0/RTCP_AF_INET (default) or RTCP_AF_INET6. */
    uint8_t family;
    size_t max_body_bytes;
    airplay_screen_message_cb_t on_message;
    void *message_context;
    airplay_screen_server_exit_cb_t on_unexpected_exit;
    void *exit_context;
} airplay_screen_server_config_t;

int airplay_screen_server_start(const struct rtcp_os *os,
                                const struct rtcp_net *net,
                                const uint8_t output_key[AIRPLAY_SCREEN_KEY_BYTES],
                                const airplay_screen_server_config_t *config,
                                airplay_screen_server_t **out_server);

uint16_t airplay_screen_server_get_port(const airplay_screen_server_t *server);
int airplay_screen_server_get_fd(const airplay_screen_server_t *server);

void airplay_screen_server_get_stats(const airplay_screen_server_t *server,
                                      airplay_screen_server_stats_t *stats);
void airplay_screen_server_poll(airplay_screen_server_t *server);
void airplay_screen_server_stop(airplay_screen_server_t *server,
                                const struct rtcp_net *net);
