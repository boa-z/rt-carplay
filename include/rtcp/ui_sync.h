/* SPDX-License-Identifier: GPL-3.0-or-later
 * Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI); see NOTICE.
 */
/** @file ui_sync.h
 * Experimental UI Sync processor and host presentation handoff. */
#ifndef RTCP_UI_SYNC_H
#define RTCP_UI_SYNC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rtcp/data_stream.h"
#ifdef __cplusplus
extern "C"
{
#endif
#define RTCP_UI_SYNC_MAX_ID          64U
#define RTCP_UI_SYNC_MAX_ACTIVE      8U
#define RTCP_UI_SYNC_MAX_MESSAGE     8192U
#define RTCP_UI_SYNC_MAX_OUTPUTS     4U
#define RTCP_UI_SYNC_MAX_LAYOUTS     8U
#define RTCP_UI_SYNC_MAX_PALETTES    8U
#define RTCP_UI_SYNC_MAX_COMPONENTS  16U
#define RTCP_UI_SYNC_MAX_CHOICES     16U
#define RTCP_UI_SYNC_DATASTREAM_UUID RTCP_DATA_STREAM_UI_SYNC_UUID
/** Component selected for a named zone. Both identifiers are bounded ASCII strings. */
typedef struct
{
    const char *zone, *component;
} rtcp_ui_sync_component_t;
/** Requested layout, palette and active zone selections. */
typedef struct
{
    const char *id, *palette;
    const rtcp_ui_sync_component_t *active;
    size_t active_count;
} rtcp_ui_sync_layout_t;
/** Host inventory for one actual display/theme. Component pairs are allowed
 * alternatives, not a Cartesian product of all zones and all components.
 * This catalogue contains identifiers only, never Apple theme assets. */
typedef struct
{
    const char *id;
    const char * const *palettes;
    size_t palette_count;
    const rtcp_ui_sync_component_t *components;
    size_t component_count;
} rtcp_ui_sync_inventory_layout_t;
/** Display and theme inventory copied by rtcp_ui_sync_bind_inventory(). */
typedef struct
{
    size_t struct_size;
    const char *theme_id, *panel_uid, *display_uuid;
    uint16_t width_pixels, height_pixels, width_physical_mm, height_physical_mm;
    const rtcp_ui_sync_inventory_layout_t *layouts;
    size_t layout_count;
} rtcp_ui_sync_inventory_t;
/** Creation options. Set struct_size to the size of the supplied structure. */
typedef struct
{
    size_t struct_size;
    const char *display_uuid;
    bool initial_dark, allow_layout_change;
    /* No reference hardcoded token is assumed. Opt in with an actual host
     * focus token; values must fit INT64_MAX. No renderer is provided here. */
    bool give_focus;
    uint64_t focus_token;
    /* Optional copied FIFO for host renderer handoff. False preserves the
     * snapshot-only behavior and allocates no choice-queue workspace. */
    bool queue_phone_choices;
} rtcp_ui_sync_config_t;
/** Protocol reset and readiness state on the serialized owner. */
typedef enum
{
    RTCP_UI_SYNC_IDLE,
    RTCP_UI_SYNC_RESETTING,
    RTCP_UI_SYNC_READY
} rtcp_ui_sync_state_t;
/** Phone appearance preference. */
typedef enum
{
    RTCP_UI_SYNC_AUTOMATIC,
    RTCP_UI_SYNC_LIGHT,
    RTCP_UI_SYNC_DARK
} rtcp_ui_sync_appearance_t;
/** Which phone selection a queued choice contains. */
typedef enum
{
    RTCP_UI_SYNC_CHOICE_LAYOUT,
    RTCP_UI_SYNC_CHOICE_PALETTE,
    RTCP_UI_SYNC_CHOICE_APPEARANCE
} rtcp_ui_sync_choice_kind_t;
/** Copied host handoff record. Its validity is scoped to its generation. */
typedef struct
{
    rtcp_ui_sync_choice_kind_t kind;
    /* Local generation distinguishes reset/reopened channels that reuse the
     * same wire session/psn. It is not a timestamp or presentation clock. */
    uint64_t generation, session, sequence;
    char layout_id[RTCP_UI_SYNC_MAX_ID + 1], palette_id[RTCP_UI_SYNC_MAX_ID + 1];
    rtcp_ui_sync_appearance_t appearance; /* Valid only for APPEARANCE. */
} rtcp_ui_sync_choice_t;
/** Snapshot of protocol, output and phone choice state on the owner. */
typedef struct
{
    rtcp_ui_sync_state_t state;
    unsigned version;
    uint64_t session, next_tx, last_rx, peer_ack;
    size_t pending_outputs;
    bool requested_dark;
    char requested_layout[RTCP_UI_SYNC_MAX_ID + 1], requested_palette[RTCP_UI_SYNC_MAX_ID + 1];
    char phone_layout[RTCP_UI_SYNC_MAX_ID + 1], phone_palette_layout[RTCP_UI_SYNC_MAX_ID + 1];
    char phone_palette[RTCP_UI_SYNC_MAX_ID + 1];
    rtcp_ui_sync_appearance_t phone_appearance;
    uint64_t received, rejected, stale, duplicates, ignored, phone_choices;
    uint64_t generated_messages, consumed_messages, abandoned_messages;
    int last_error;
    bool choice_queue_enabled;
    size_t pending_choices;
    uint64_t choice_generation, consumed_choices, abandoned_choices;
} rtcp_ui_sync_status_t;
/** Opaque UI Sync processor restricted to a single serialized owner. */
typedef struct rtcp_ui_sync rtcp_ui_sync_t;
/** Receiver delivery counters for the attached UI Sync channel. */
typedef struct
{
    bool attached;
    uint64_t queued_messages, queue_retries, rejected_requests;
    int last_error;
    /* Exact application/delivery refusal before lane retirement. The generic
     * channel-close error may replace last_error; it must not erase this
     * cause. Cumulative until the next rejected request or Receiver destroy. */
    int last_rejection;
} rtcp_ui_sync_delivery_status_t;

/** Create an owned UI Sync processor on one serialized owner. Configuration/layout strings are
 * copied. Bounded foundation: nonempty ASCII identifiers <=64 bytes, eight
 * active components, panel metadata <=1024 encoded bytes. No capability is
 * advertised and no official renderer/timing/retransmission policy is implied. */
int rtcp_ui_sync_create(const rtcp_ui_sync_config_t *, rtcp_ui_sync_t **);
/** Free the processor after detaching it from the Receiver. */
void rtcp_ui_sync_destroy(rtcp_ui_sync_t *);
/** Validate and copy inventory before channel negotiation. NULL unbinds it.
 * Only IDLE with no retained output may bind/unbind; an existing desired
 * layout must also be supported by a replacement. A failed bind is atomic.
 * With a bound inventory, select and phone layout/palette choices outside it
 * return ENOTSUP before committing state or generating ACK. Reset retains it.
 * An unbound model retains the original bounded protocol-foundation behavior.
 * Binding does not advertise /info, enable uiSync, authenticate theme assets,
 * install a renderer or prove presentation. The host must implement those
 * separate capabilities before opting into phone-facing declarations. */
int rtcp_ui_sync_bind_inventory(rtcp_ui_sync_t *, const rtcp_ui_sync_inventory_t *);
/** Borrowed read-only view of copied identifiers; NULL when unbound. Valid
 * until successful rebind/unbind/destroy, on the same serialized owner. */
const rtcp_ui_sync_inventory_t *rtcp_ui_sync_get_inventory(const rtcp_ui_sync_t *);
/** Clear channel/session/output/phone state, retain requested layout/dark mode
 * and cumulative counters. Call at each authenticated session/channel reset. */
void rtcp_ui_sync_reset_session(rtcp_ui_sync_t *);
/** Copy session and delivery counters on the owner. Returns RTCP_EINVAL
 * for NULL input or output. */
int rtcp_ui_sync_status(const rtcp_ui_sync_t *, rtcp_ui_sync_status_t *);
/** Copy the desired layout and stage its update when active. Returns a
 * negative RTCP error for invalid/unavailable layout or pending output. */
int rtcp_ui_sync_select(rtcp_ui_sync_t *, const rtcp_ui_sync_layout_t *);
/** Set desired appearance and stage its update when active. Returns
 * RTCP_ESTATE if pending output prevents the update. */
int rtcp_ui_sync_set_dark(rtcp_ui_sync_t *, bool);
/** Optional host handoff FIFO: peek copies the oldest accepted choice without
 * removing it. Consume only after the host has copied/accepted that choice.
 * Neither operation means a frame was rendered/presented. Both are owner-only;
 * do not interleave handle/reset between a peek and its corresponding consume.
 * ENOTSUP when disabled, ENOENT when empty. Peek clears output on failure.
 * One layout command with a palette stages two records atomically. A full
 * queue returns ENOMEM before choice/state/ACK commit, never drops an oldest
 * record. Protocol RESET and reset_session abandon pending choices visibly;
 * stale/duplicate commands do not enqueue them again. No renderer, callback,
 * presentation timestamp or wire timing policy is supplied by this API.
 * Host queues must invalidate older generations when reset/reopen is observed.
 * Generation exhaustion stops new queued choices with ESTATE until recreate. */
int rtcp_ui_sync_peek_choice(const rtcp_ui_sync_t *, rtcp_ui_sync_choice_t *);
/** Remove the oldest choice after host acceptance. Returns RTCP_ENOTSUP
 * when disabled or RTCP_ENOENT when empty. */
int rtcp_ui_sync_consume_choice(rtcp_ui_sync_t *);
/** Handle a complete authenticated Type 130 body: OPACK panel + binary plist header
 * + optional nested binary plist command. Caller retains input until return.
 * Malformed input/output exhaustion do not commit protocol state or outputs.
 * Pending output yields ESTATE before processing a new request/selection. */
int rtcp_ui_sync_handle(rtcp_ui_sync_t *, const uint8_t *, size_t);
/** Peek borrows an encoded message until consume/reset/destroy. Only consume
 * after the transport copied it; queue exhaustion leaves bytes untouched.
 * ENOENT means empty. Consumption is not phone receipt or application ACK. */
int rtcp_ui_sync_peek(const rtcp_ui_sync_t *, const uint8_t **, size_t *);
/** Release pending output after the transport copied it. Returns
 * RTCP_ENOENT when there is no output to consume. */
int rtcp_ui_sync_consume(rtcp_ui_sync_t *);
#ifdef __cplusplus
}
#endif
#endif
