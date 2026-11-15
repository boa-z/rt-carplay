/* SPDX-License-Identifier: GPL-3.0-or-later
 * Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI); see NOTICE.
 */
#ifndef RTCP_UI_SYNC_H
#define RTCP_UI_SYNC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rtcp/data_stream.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RTCP_UI_SYNC_MAX_ID 64U
#define RTCP_UI_SYNC_MAX_ACTIVE 8U
#define RTCP_UI_SYNC_MAX_MESSAGE 8192U
#define RTCP_UI_SYNC_MAX_OUTPUTS 4U
#define RTCP_UI_SYNC_DATASTREAM_UUID RTCP_DATA_STREAM_UI_SYNC_UUID
typedef struct { const char *zone, *component; } rtcp_ui_sync_component_t;
typedef struct {
    const char *id, *palette;
    const rtcp_ui_sync_component_t *active;
    size_t active_count;
} rtcp_ui_sync_layout_t;
typedef struct {
    size_t struct_size;
    const char *display_uuid;
    bool initial_dark, allow_layout_change;
    /* No reference hardcoded token is assumed. Opt in with an actual host
     * focus token; values must fit INT64_MAX. No renderer is provided here. */
    bool give_focus;
    uint64_t focus_token;
} rtcp_ui_sync_config_t;
typedef enum { RTCP_UI_SYNC_IDLE, RTCP_UI_SYNC_RESETTING, RTCP_UI_SYNC_READY } rtcp_ui_sync_state_t;
typedef enum { RTCP_UI_SYNC_AUTOMATIC, RTCP_UI_SYNC_LIGHT, RTCP_UI_SYNC_DARK } rtcp_ui_sync_appearance_t;
typedef struct {
    rtcp_ui_sync_state_t state;
    unsigned version;
    uint64_t session, next_tx, last_rx, peer_ack;
    size_t pending_outputs;
    bool requested_dark;
    char requested_layout[RTCP_UI_SYNC_MAX_ID+1], requested_palette[RTCP_UI_SYNC_MAX_ID+1];
    char phone_layout[RTCP_UI_SYNC_MAX_ID+1], phone_palette_layout[RTCP_UI_SYNC_MAX_ID+1];
    char phone_palette[RTCP_UI_SYNC_MAX_ID+1];
    rtcp_ui_sync_appearance_t phone_appearance;
    uint64_t received, rejected, stale, duplicates, ignored, phone_choices;
    uint64_t generated_messages, consumed_messages, abandoned_messages;
    int last_error;
} rtcp_ui_sync_status_t;
typedef struct rtcp_ui_sync rtcp_ui_sync_t;
typedef struct {
    bool attached;
    uint64_t queued_messages, queue_retries, rejected_requests;
    int last_error;
} rtcp_ui_sync_delivery_status_t;

/* One serialized owner, no internal thread. Configuration/layout strings are
 * copied. Bounded foundation: nonempty ASCII identifiers <=64 bytes, eight
 * active components, panel metadata <=1024 encoded bytes. No capability is
 * advertised and no official renderer/timing/retransmission policy is implied. */
int rtcp_ui_sync_create(const rtcp_ui_sync_config_t *, rtcp_ui_sync_t **);
void rtcp_ui_sync_destroy(rtcp_ui_sync_t *);
/* Clear channel/session/output/phone state, retain requested layout/dark mode
 * and cumulative counters. Call at each authenticated session/channel reset. */
void rtcp_ui_sync_reset_session(rtcp_ui_sync_t *);
int rtcp_ui_sync_status(const rtcp_ui_sync_t *, rtcp_ui_sync_status_t *);
int rtcp_ui_sync_select(rtcp_ui_sync_t *, const rtcp_ui_sync_layout_t *);
int rtcp_ui_sync_set_dark(rtcp_ui_sync_t *, bool);
/* Complete authenticated Type 130 comm body: OPACK panel + binary plist header
 * + optional nested binary plist command. Caller retains input until return.
 * Malformed input/output exhaustion do not commit protocol state or outputs.
 * Pending output yields ESTATE before processing a new request/selection. */
int rtcp_ui_sync_handle(rtcp_ui_sync_t *, const uint8_t *, size_t);
/* Peek borrows an encoded message until consume/reset/destroy. Only consume
 * after the transport copied it; queue exhaustion leaves bytes untouched.
 * ENOENT means empty. Consumption is not phone receipt or application ACK. */
int rtcp_ui_sync_peek(const rtcp_ui_sync_t *, const uint8_t **, size_t *);
int rtcp_ui_sync_consume(rtcp_ui_sync_t *);
#ifdef __cplusplus
}
#endif
#endif
