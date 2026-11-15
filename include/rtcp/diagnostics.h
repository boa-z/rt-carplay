/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_DIAGNOSTICS_H
#define RTCP_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

/* Owner-thread snapshots. Elapsed wall time includes callbacks/preemption;
 * counters do not establish presentation quality or clock synchronization. */
typedef struct {
    uint32_t recv_calls;
    uint32_t owner_calls, owner_gap_over100ms;
    uint32_t owner_gap_max_us; /* lifetime maxima, explicitly labeled */
    uint32_t partial_gaps, partial_gap_over100ms, partial_gap_max_us;
    uint32_t recv_block_over10ms, recv_block_max_us;
    uint32_t pending_body_bytes, pending_header_bytes, pending_age_ms;
    uint32_t read_batches;       /* owner dispatches, excluding accept */
    uint32_t read_budget_yields; /* 16 reads or 2 ms; callback may overrun */
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
} rtcp_screen_stats_t;

/* Experimental alternate transport, owner-thread snapshot. Counters survive
 * selective close until next open; open/close counts reset on stop/start.
 * last_error is transport-level EIO on unexpected loss, not a codec verdict. */
typedef struct {
    bool configured, ready;
    uint16_t port;
    uint32_t opened, closed;
    int last_error;
    rtcp_screen_stats_t screen;
} rtcp_alt_video_status_t;

typedef struct {
    bool active;
    uint32_t requests_sent, responses_received;
    uint32_t requests_received, responses_sent;
    uint32_t rejected, send_errors, timeouts;
} rtcp_timing_stats_t;

typedef struct {
    unsigned active_streams;
    uint32_t datagrams, auth_failures;
    uint32_t malformed, replay_drops, delivered, sink_rejected;
    /* Authenticated sequence high-water observations, not proven network loss.
     * Reordering/retransmission can subsequently fill forward gaps. */
    uint32_t sequence_missing, sequence_reordered;
    uint32_t poll_gap_max_us;
} rtcp_audio_stats_t;

/* datagrams precede authentication; active_streams counts active Audio SETUP.
 * Sink packet counters separately describe payloads accepted by the host. */
typedef struct {
    rtcp_screen_stats_t screen;
    rtcp_audio_stats_t audio;
    rtcp_timing_stats_t timing;
    bool single_cipher_frame;
} rtcp_receiver_diagnostics_t;

#endif
