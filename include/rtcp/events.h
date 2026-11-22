/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file events.h
 * Receiver state snapshots and owner-thread notifications. */
#ifndef RTCP_EVENTS_H
#define RTCP_EVENTS_H
#include <stdbool.h>
#include <stdint.h>

/** Copied negotiation and transport status. Endpoint readiness describes
 * setup, with media receipt measured separately by diagnostics. */
typedef struct
{
    bool running;
    bool transport_connected; /**< RTSP socket accepted. */
    bool authenticated;       /**< Pair-Verify and ControlCipher completed. */
    bool session_established; /**< RECORD accepted. */
    bool video_ready;         /**< Negotiated Screen endpoint exists. */
    bool audio_ready;         /**< At least one negotiated audio endpoint exists. */
    bool input_ready;         /**< Encrypted event channel attached. */
    uint32_t generation;      /**< Successful receiver starts, preserved on stop. */
    int last_error;
} rtcp_receiver_state_t;

/** Called synchronously on the lifecycle/poll owner when state changes.
 * state is borrowed until return; copy it to notify another thread. Querying
 * state is permitted here; reentrant lifecycle calls return RTCP_ESTATE. */
typedef void (*rtcp_receiver_event_cb_t)(const rtcp_receiver_state_t *state,
                                         void *context);

#endif
