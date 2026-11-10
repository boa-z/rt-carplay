/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2025-2026 DiPlay contributors
 * Copyright (C) 2026 ESP32 CarPlay Web Relay contributors
 *
 * The bounded iAP2 link state machine follows the protocol structure in DiPlay's
 * Iap2LinkEngine.kt. See THIRD_PARTY_NOTICES.md and COPYING for attribution and terms.
 */
#include "iap2_core.h"

#include <stdlib.h>
#include <string.h>

/*
 * The link core follows the original ESP-IDF logging call sites, but routes
 * them through the portable rtcp/log.h seam instead of a fixed no-op: a
 * platform build with RTCP_LOG_CUSTOM (the RT-Thread port) then gets the
 * message trace (csm_rx/csm_tx ids, sync limits, link state) needed to
 * diagnose a live pairing, while a silent build costs nothing.
 */
#include "rtcp/log.h"

#define ESP_LOGD(tag, format, ...) RTCP_LOGD(tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) RTCP_LOGI(tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) RTCP_LOGW(tag, format, ##__VA_ARGS__)

#define IAP2_LINK_HEADER_BYTES 9U
#define IAP2_LINK_CHECKSUM_BYTES 1U
#define IAP2_MAX_LINK_FRAME_BYTES 2048U
#define IAP2_MAX_CSM_FRAME_BYTES 2048U
#define IAP2_MAX_CSM_BODY_BYTES (IAP2_MAX_CSM_FRAME_BYTES - 6U)
#define IAP2_CONTROL_SESSION_ID 10U
#define IAP2_INITIAL_SEQUENCE 99U
#define IAP2_CONTROL_SYNCHRONIZE 0x80U
#define IAP2_CONTROL_ACKNOWLEDGEMENT 0x40U
#define IAP2_CONTROL_EXTENDED_ACKNOWLEDGEMENT 0x20U
#define IAP2_CONTROL_RESET 0x10U
#define IAP2_MARKER_RESEND_MS 1000U
#define IAP2_SYNC_RESEND_MS 500U
#define IAP2_RETRANSMIT_MS 4000U
#define IAP2_MAX_RETRANSMISSIONS 4U
#define IAP2_LOCAL_MAX_OUTGOING 1U
#define IAP2_LOCAL_MAX_ACKS 1U
#define IAP2_PENDING_CSM_DEPTH 8U
#define IAP2_PENDING_CSM_BODY_BYTES 512U

static const char *TAG = "iap2_core";
static const uint8_t IAP2_MARKER[] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};

typedef struct {
    uint16_t message_id;
    uint16_t body_len;
    iap2_core_csm_acked_cb_t acked_callback;
    void *acked_context;
    uint8_t body[IAP2_PENDING_CSM_BODY_BYTES];
} pending_csm_t;

struct iap2_core {
    iap2_core_tx_cb_t tx_cb;
    iap2_core_message_cb_t message_cb;
    iap2_core_state_cb_t state_cb;
    void *context;
    iap2_core_state_t state;
    uint8_t rx[IAP2_MAX_LINK_FRAME_BYTES];
    size_t rx_len;
    uint8_t csm_rx[IAP2_MAX_CSM_FRAME_BYTES];
    size_t csm_rx_len;
    uint8_t retransmit[IAP2_MAX_LINK_FRAME_BYTES];
    size_t retransmit_len;
    uint16_t peer_max_length;
    uint8_t peer_max_outgoing;
    uint8_t sent_sequence;
    uint8_t last_rx_sequence;
    uint8_t waiting_sequence;
    uint16_t waiting_message_id;
    iap2_core_csm_acked_cb_t waiting_acked_callback;
    void *waiting_acked_context;
    uint8_t retries;
    bool peer_sync_received;
    bool waiting_ack;
    uint64_t marker_deadline_ms;
    uint64_t sync_deadline_ms;
    uint64_t tx_deadline_ms;
    uint32_t rx_frames;
    uint32_t bad_frames;
    pending_csm_t pending[IAP2_PENDING_CSM_DEPTH];
    size_t pending_head;
    size_t pending_count;
};

static bool send_pending_csm(iap2_core_t *core, uint64_t now_ms);

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static uint8_t checksum(const uint8_t *bytes, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return (uint8_t)(0U - sum);
}

static bool checksum_valid(const uint8_t *bytes, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return sum == 0;
}

static void set_state(iap2_core_t *core, iap2_core_state_t state, const char *reason)
{
    if (core->state == state) {
        return;
    }
    core->state = state;
    if (core->state_cb != NULL) {
        core->state_cb(state, reason, core->context);
    }
}

static void fail(iap2_core_t *core, const char *reason)
{
    if (core->state == IAP2_CORE_DEAD) {
        return;
    }
    core->marker_deadline_ms = 0;
    core->sync_deadline_ms = 0;
    core->tx_deadline_ms = 0;
    core->waiting_ack = false;
    core->waiting_message_id = 0U;
    core->waiting_acked_callback = NULL;
    core->waiting_acked_context = NULL;
    core->pending_head = 0;
    core->pending_count = 0;
    core->retransmit_len = 0;
    core->rx_len = 0;
    core->csm_rx_len = 0;
    ESP_LOGW(TAG, "link=dead reason=%s", reason != NULL ? reason : "peer_closed");
    set_state(core, IAP2_CORE_DEAD, reason);
}

static bool transmit(iap2_core_t *core, const uint8_t *bytes, size_t len)
{
    if (core->tx_cb == NULL || !core->tx_cb(bytes, len, core->context)) {
        ESP_LOGW(TAG, "link_tx=failed bytes=%u", (unsigned)len);
        return false;
    }
    return true;
}

static bool write_link_frame(iap2_core_t *core,
                             const uint8_t *payload,
                             size_t payload_len,
                             uint8_t control,
                             uint8_t sequence,
                             uint8_t session_id,
                             uint8_t *out,
                             size_t out_capacity,
                             size_t *out_len)
{
    size_t length = IAP2_LINK_HEADER_BYTES;
    if (payload != NULL) {
        length += payload_len + IAP2_LINK_CHECKSUM_BYTES;
    }
    if (length > IAP2_MAX_LINK_FRAME_BYTES || length > out_capacity || length > UINT16_MAX) {
        return false;
    }
    out[0] = 0xff;
    out[1] = 0x5a;
    write_be16(out + 2, (uint16_t)length);
    out[4] = control;
    out[5] = sequence;
    out[6] = core->last_rx_sequence;
    out[7] = session_id;
    out[8] = checksum(out, IAP2_LINK_HEADER_BYTES - 1U);
    if (payload != NULL) {
        if (payload_len != 0) {
            memcpy(out + IAP2_LINK_HEADER_BYTES, payload, payload_len);
        }
        out[length - 1U] = checksum(payload, payload_len);
    }
    *out_len = length;
    return true;
}

static bool send_link_frame(iap2_core_t *core,
                            const uint8_t *payload,
                            size_t payload_len,
                            uint8_t control,
                            uint8_t sequence,
                            uint8_t session_id)
{
    uint8_t frame[IAP2_MAX_LINK_FRAME_BYTES];
    size_t frame_len = 0;
    if (!write_link_frame(core, payload, payload_len, control, sequence, session_id,
                          frame, sizeof(frame), &frame_len)) {
        return false;
    }
    return transmit(core, frame, frame_len);
}

static bool send_sync(iap2_core_t *core)
{
    uint8_t payload[19] = {
        1, IAP2_LOCAL_MAX_OUTGOING,
        0, 0, /* maximum frame length, filled below */
        0x0f, 0xa0, /* retransmission timeout: 4000 ms */
        0x01, 0xf4, /* acknowledgement timeout: 500 ms */
        IAP2_MAX_RETRANSMISSIONS, IAP2_LOCAL_MAX_ACKS,
        10, 0, 1, /* control session */
        11, 2, 1, /* EA session */
        12, 1, 2, /* file transfer session */
    };
    write_be16(payload + 2, IAP2_MAX_LINK_FRAME_BYTES);
    return send_link_frame(core, payload, sizeof(payload), IAP2_CONTROL_SYNCHRONIZE,
                           IAP2_INITIAL_SEQUENCE, 0);
}

static bool send_ack(iap2_core_t *core)
{
    return send_link_frame(core, NULL, 0, IAP2_CONTROL_ACKNOWLEDGEMENT,
                           core->sent_sequence, 0);
}

static bool send_extended_ack(iap2_core_t *core, uint8_t first_missing,
                              uint8_t missing_count)
{
    if (core == NULL || missing_count == 0U) return false;
    uint8_t missing[UINT8_MAX];
    for (uint16_t i = 0U; i < missing_count; ++i) {
        missing[i] = (uint8_t)(first_missing + i);
    }
    return send_link_frame(core, missing, missing_count,
                           IAP2_CONTROL_EXTENDED_ACKNOWLEDGEMENT,
                           core->sent_sequence, 0);
}

static void retransmit_requested(iap2_core_t *core, uint64_t now_ms)
{
    if (!core->waiting_ack || core->retransmit_len == 0U) return;
    if (core->retries >= IAP2_MAX_RETRANSMISSIONS) {
        fail(core, "link_ack_timeout");
        return;
    }
    ++core->retries;
    core->tx_deadline_ms = now_ms + IAP2_RETRANSMIT_MS;
    if (!transmit(core, core->retransmit, core->retransmit_len)) {
        fail(core, "link_retransmit_failed");
        return;
    }
    ESP_LOGW(TAG, "link_tx=retransmitted sequence=%u retry=%u reason=extended_ack",
             core->waiting_sequence, core->retries);
}

static void csm_consume(iap2_core_t *core, const uint8_t *bytes, size_t len)
{
    size_t offset = 0;
    while (offset < len && core->state != IAP2_CORE_DEAD) {
        if (core->csm_rx_len == sizeof(core->csm_rx)) {
            fail(core, "csm_receive_buffer_full");
            return;
        }
        size_t room = sizeof(core->csm_rx) - core->csm_rx_len;
        size_t count = len - offset < room ? len - offset : room;
        memcpy(core->csm_rx + core->csm_rx_len, bytes + offset, count);
        core->csm_rx_len += count;
        offset += count;

        while (core->csm_rx_len >= 2) {
            if (core->csm_rx[0] != 0x40 || core->csm_rx[1] != 0x40) {
                memmove(core->csm_rx, core->csm_rx + 1, --core->csm_rx_len);
                ++core->bad_frames;
                continue;
            }
            if (core->csm_rx_len < 6) {
                break;
            }
            uint16_t frame_len = read_be16(core->csm_rx + 2);
            if (frame_len < 6 || frame_len > sizeof(core->csm_rx)) {
                memmove(core->csm_rx, core->csm_rx + 1, --core->csm_rx_len);
                ++core->bad_frames;
                continue;
            }
            if (core->csm_rx_len < frame_len) {
                break;
            }
            uint16_t message_id = read_be16(core->csm_rx + 4);
            ++core->rx_frames;
            ESP_LOGI(TAG, "csm_rx id=0x%04x bytes=%u", message_id, (unsigned)(frame_len - 6U));
            if (core->message_cb != NULL) {
                core->message_cb(message_id, core->csm_rx + 6, frame_len - 6U, core->context);
            }
            core->csm_rx_len -= frame_len;
            if (core->csm_rx_len != 0) {
                memmove(core->csm_rx, core->csm_rx + frame_len, core->csm_rx_len);
            }
        }
    }
}

static void handle_frame(iap2_core_t *core,
                         const uint8_t *frame,
                         size_t frame_len,
                         uint64_t now_ms)
{
    uint8_t control = frame[4];
    uint8_t sequence = frame[5];
    uint8_t acknowledgement = frame[6];
    uint8_t session_id = frame[7];
    bool has_payload = frame_len > IAP2_LINK_HEADER_BYTES;
    const uint8_t *payload = has_payload ? frame + IAP2_LINK_HEADER_BYTES : NULL;
    size_t payload_len = has_payload ? frame_len - IAP2_LINK_HEADER_BYTES - 1U : 0;

    if ((control & IAP2_CONTROL_RESET) != 0) {
        fail(core, "peer_sent_link_reset");
        return;
    }
    if ((control & IAP2_CONTROL_SYNCHRONIZE) != 0) {
        if (core->state != IAP2_CORE_NEGOTIATING) {
            ++core->bad_frames;
            ESP_LOGW(TAG, "sync_rx=unexpected state=%u", (unsigned)core->state);
        } else if (payload_len < 10 || payload[0] != 1) {
            ++core->bad_frames;
            ESP_LOGW(TAG, "sync_rx=invalid bytes=%u", (unsigned)payload_len);
            return;
        } else {
            core->peer_max_outgoing = payload[1];
            core->peer_max_length = read_be16(payload + 2);
            core->peer_sync_received = true;
            core->last_rx_sequence = sequence;
            if (core->peer_max_outgoing == 0 || core->peer_max_length <= 10) {
                fail(core, "peer_iap2_limits_invalid");
                return;
            }
            ESP_LOGI(TAG, "sync_rx max_outgoing=%u max_length=%u",
                     core->peer_max_outgoing, core->peer_max_length);
            if (!send_ack(core)) {
                fail(core, "sync_ack_send_failed");
                return;
            }
        }
    }
    if ((control & IAP2_CONTROL_ACKNOWLEDGEMENT) != 0) {
        if (core->state == IAP2_CORE_NEGOTIATING && core->peer_sync_received &&
            acknowledgement == IAP2_INITIAL_SEQUENCE) {
            core->sync_deadline_ms = 0;
            core->sent_sequence = IAP2_INITIAL_SEQUENCE;
            set_state(core, IAP2_CORE_READY, "link_synchronized");
            ESP_LOGI(TAG, "link=ready peer_max_length=%u", core->peer_max_length);
        } else if (core->waiting_ack && acknowledgement == core->waiting_sequence) {
            core->waiting_ack = false;
            core->tx_deadline_ms = 0;
            core->retries = 0;
            iap2_core_csm_acked_cb_t acked_callback = core->waiting_acked_callback;
            uint16_t acked_message_id = core->waiting_message_id;
            void *acked_context = core->waiting_acked_context;
            core->waiting_message_id = 0U;
            core->waiting_acked_callback = NULL;
            core->waiting_acked_context = NULL;
            if (acked_callback != NULL) {
                acked_callback(acked_message_id, acked_context);
            }
            (void)send_pending_csm(core, now_ms);
        }
    }
    if ((control & IAP2_CONTROL_EXTENDED_ACKNOWLEDGEMENT) != 0) {
        if (!has_payload || payload_len == 0U) {
            ++core->bad_frames;
            ESP_LOGW(TAG, "extended_ack_rx=empty");
        } else if (core->state == IAP2_CORE_READY) {
            for (size_t i = 0U; i < payload_len; ++i) {
                if (payload[i] == core->waiting_sequence) {
                    retransmit_requested(core, now_ms);
                    break;
                }
            }
        }
    }
    /* Only a plain ACK control frame carries session data. Combined SYNC/ACK or
       extended-ACK/ACK payloads belong to link control, not the CSM stream. */
    if (has_payload && control == IAP2_CONTROL_ACKNOWLEDGEMENT &&
        core->state == IAP2_CORE_READY) {
        uint8_t distance = (uint8_t)(sequence - core->last_rx_sequence);
        if (distance == 1U) {
            core->last_rx_sequence = sequence;
            if (session_id == IAP2_CONTROL_SESSION_ID) {
                csm_consume(core, payload, payload_len);
            } else {
                ESP_LOGD(TAG, "session_rx=ignored id=%u bytes=%u", session_id,
                         (unsigned)payload_len);
            }
        }
        bool ack_sent;
        uint16_t remote_window = (uint16_t)core->peer_max_outgoing + 10U;
        if (distance >= 2U && (uint16_t)distance <= remote_window) {
            /* We do not buffer out-of-order CSM packets. Ask the peer to resend the
               missing range, including this packet, so it cannot be silently lost. */
            ack_sent = send_extended_ack(core,
                                         (uint8_t)(core->last_rx_sequence + 1U),
                                         distance);
        } else {
            /* This also acknowledges duplicates and stale retransmissions. */
            ack_sent = send_ack(core);
        }
        if (!ack_sent) {
            fail(core, "data_ack_send_failed");
        }
    }
    (void)now_ms;
}

static bool parse_frames(iap2_core_t *core, uint64_t now_ms)
{
    bool progressed = false;
    while (core->state != IAP2_CORE_DEAD) {
        size_t discard = 0;
        while (core->rx_len >= 2 &&
               (core->rx[0] != 0xff || core->rx[1] != 0x5a)) {
            ++discard;
            --core->rx_len;
            if (core->rx_len != 0) {
                memmove(core->rx, core->rx + 1, core->rx_len);
            }
        }
        if (discard != 0) {
            core->bad_frames += (uint32_t)discard;
            progressed = true;
        }
        if (core->rx_len < IAP2_LINK_HEADER_BYTES) {
            break;
        }
        uint16_t frame_len = read_be16(core->rx + 2);
        if (frame_len < IAP2_LINK_HEADER_BYTES || frame_len > sizeof(core->rx)) {
            memmove(core->rx, core->rx + 1, --core->rx_len);
            ++core->bad_frames;
            progressed = true;
            continue;
        }
        if (core->rx_len < frame_len) {
            break;
        }
        if (!checksum_valid(core->rx, IAP2_LINK_HEADER_BYTES) ||
            (frame_len > IAP2_LINK_HEADER_BYTES &&
             !checksum_valid(core->rx + IAP2_LINK_HEADER_BYTES,
                             frame_len - IAP2_LINK_HEADER_BYTES))) {
            memmove(core->rx, core->rx + frame_len, core->rx_len - frame_len);
            core->rx_len -= frame_len;
            ++core->bad_frames;
            progressed = true;
            continue;
        }
        handle_frame(core, core->rx, frame_len, now_ms);
        memmove(core->rx, core->rx + frame_len, core->rx_len - frame_len);
        core->rx_len -= frame_len;
        progressed = true;
    }
    return progressed;
}

iap2_core_t *iap2_core_create(iap2_core_tx_cb_t tx_cb,
                              iap2_core_message_cb_t message_cb,
                              iap2_core_state_cb_t state_cb,
                              void *context)
{
    if (tx_cb == NULL) {
        return NULL;
    }
    iap2_core_t *core = calloc(1, sizeof(*core));
    if (core == NULL) {
        return NULL;
    }
    core->tx_cb = tx_cb;
    core->message_cb = message_cb;
    core->state_cb = state_cb;
    core->context = context;
    core->state = IAP2_CORE_IDLE;
    return core;
}

void iap2_core_destroy(iap2_core_t *core)
{
    free(core);
}

void iap2_core_reset(iap2_core_t *core, const char *reason)
{
    if (core == NULL) {
        return;
    }
    core->rx_len = 0;
    core->csm_rx_len = 0;
    core->retransmit_len = 0;
    core->peer_max_length = 0;
    core->peer_max_outgoing = 0;
    core->sent_sequence = IAP2_INITIAL_SEQUENCE;
    core->last_rx_sequence = 0;
    core->waiting_sequence = 0;
    core->waiting_message_id = 0U;
    core->waiting_acked_callback = NULL;
    core->waiting_acked_context = NULL;
    core->retries = 0;
    core->peer_sync_received = false;
    core->waiting_ack = false;
    core->pending_head = 0;
    core->pending_count = 0;
    core->marker_deadline_ms = 0;
    core->sync_deadline_ms = 0;
    core->tx_deadline_ms = 0;
    set_state(core, IAP2_CORE_IDLE, reason);
}

bool iap2_core_start(iap2_core_t *core, uint64_t now_ms)
{
    if (core == NULL || core->state != IAP2_CORE_IDLE) {
        return false;
    }
    set_state(core, IAP2_CORE_WAIT_MARKER, "rfcomm_connected");
    if (!transmit(core, IAP2_MARKER, sizeof(IAP2_MARKER))) {
        fail(core, "marker_send_failed");
        return false;
    }
    core->marker_deadline_ms = now_ms + IAP2_MARKER_RESEND_MS;
    return true;
}

void iap2_core_feed(iap2_core_t *core, const uint8_t *bytes, size_t len, uint64_t now_ms)
{
    if (core == NULL || bytes == NULL || len == 0 || core->state == IAP2_CORE_DEAD ||
        core->state == IAP2_CORE_IDLE) {
        return;
    }
    size_t offset = 0;
    while (offset < len && core->state != IAP2_CORE_DEAD) {
        if (core->rx_len == sizeof(core->rx)) {
            if (!parse_frames(core, now_ms)) {
                fail(core, "link_receive_buffer_full");
                return;
            }
        }
        size_t room = sizeof(core->rx) - core->rx_len;
        size_t count = len - offset < room ? len - offset : room;
        memcpy(core->rx + core->rx_len, bytes + offset, count);
        core->rx_len += count;
        offset += count;

        if (core->state == IAP2_CORE_WAIT_MARKER) {
            if (core->rx_len < sizeof(IAP2_MARKER)) {
                continue;
            }
            if (memcmp(core->rx, IAP2_MARKER, sizeof(IAP2_MARKER)) != 0) {
                fail(core, "peer_iap2_marker_invalid");
                return;
            }
            memmove(core->rx, core->rx + sizeof(IAP2_MARKER),
                    core->rx_len - sizeof(IAP2_MARKER));
            core->rx_len -= sizeof(IAP2_MARKER);
            core->marker_deadline_ms = 0;
            set_state(core, IAP2_CORE_NEGOTIATING, "peer_marker_received");
            if (!send_sync(core)) {
                fail(core, "sync_send_failed");
                return;
            }
            core->sync_deadline_ms = now_ms + IAP2_SYNC_RESEND_MS;
        }
        (void)parse_frames(core, now_ms);
    }
}

void iap2_core_advance_time(iap2_core_t *core, uint64_t now_ms)
{
    if (core == NULL || core->state == IAP2_CORE_DEAD) {
        return;
    }
    if (core->state == IAP2_CORE_WAIT_MARKER && now_ms >= core->marker_deadline_ms) {
        core->marker_deadline_ms = now_ms + IAP2_MARKER_RESEND_MS;
        (void)transmit(core, IAP2_MARKER, sizeof(IAP2_MARKER));
    }
    if (core->state == IAP2_CORE_NEGOTIATING && now_ms >= core->sync_deadline_ms) {
        core->sync_deadline_ms = now_ms + IAP2_SYNC_RESEND_MS;
        if (!send_sync(core)) {
            fail(core, "sync_retry_failed");
        }
    }
    if (core->state == IAP2_CORE_READY && core->waiting_ack && now_ms >= core->tx_deadline_ms) {
        retransmit_requested(core, now_ms);
    }
}

bool iap2_core_send_csm_with_ack_callback(iap2_core_t *core,
                                          uint16_t message_id,
                                          const uint8_t *body,
                                          size_t body_len,
                                          iap2_core_csm_acked_cb_t acked_callback,
                                          void *acked_context,
                                          uint64_t now_ms)
{
    if (core == NULL || (body == NULL && body_len != 0) || core->state != IAP2_CORE_READY ||
        body_len > IAP2_MAX_CSM_BODY_BYTES) {
        return false;
    }
    if (core->waiting_ack) {
        if (body_len > IAP2_PENDING_CSM_BODY_BYTES ||
            core->pending_count >= IAP2_PENDING_CSM_DEPTH) {
            return false;
        }
        size_t tail = (core->pending_head + core->pending_count) % IAP2_PENDING_CSM_DEPTH;
        pending_csm_t *pending = &core->pending[tail];
        pending->message_id = message_id;
        pending->body_len = (uint16_t)body_len;
        pending->acked_callback = acked_callback;
        pending->acked_context = acked_context;
        if (body_len != 0) {
            memcpy(pending->body, body, body_len);
        }
        ++core->pending_count;
        return true;
    }
    size_t csm_len = body_len + 6U;
    size_t peer_payload_limit = core->peer_max_length - IAP2_LINK_HEADER_BYTES - 1U;
    if (csm_len > peer_payload_limit || csm_len + IAP2_LINK_HEADER_BYTES + 1U > sizeof(core->retransmit)) {
        return false;
    }
    uint8_t csm[IAP2_MAX_CSM_FRAME_BYTES];
    csm[0] = 0x40;
    csm[1] = 0x40;
    write_be16(csm + 2, (uint16_t)csm_len);
    write_be16(csm + 4, message_id);
    if (body_len != 0) {
        memcpy(csm + 6, body, body_len);
    }
    core->sent_sequence = (uint8_t)(core->sent_sequence + 1U);
    size_t link_len = 0;
    if (!write_link_frame(core, csm, csm_len, IAP2_CONTROL_ACKNOWLEDGEMENT,
                          core->sent_sequence, IAP2_CONTROL_SESSION_ID,
                          core->retransmit, sizeof(core->retransmit), &link_len)) {
        return false;
    }
    if (!transmit(core, core->retransmit, link_len)) {
        fail(core, "csm_send_failed");
        return false;
    }
    core->retransmit_len = link_len;
    core->waiting_sequence = core->sent_sequence;
    core->waiting_message_id = message_id;
    core->waiting_acked_callback = acked_callback;
    core->waiting_acked_context = acked_context;
    core->waiting_ack = true;
    core->retries = 0;
    core->tx_deadline_ms = now_ms + IAP2_RETRANSMIT_MS;
    ESP_LOGI(TAG, "csm_tx id=0x%04x bytes=%u sequence=%u",
             message_id, (unsigned)body_len, core->sent_sequence);
    return true;
}

bool iap2_core_send_csm(iap2_core_t *core,
                        uint16_t message_id,
                        const uint8_t *body,
                        size_t body_len,
                        uint64_t now_ms)
{
    return iap2_core_send_csm_with_ack_callback(core, message_id, body, body_len,
                                                NULL, NULL, now_ms);
}

static bool send_pending_csm(iap2_core_t *core, uint64_t now_ms)
{
    if (core == NULL || core->waiting_ack || core->pending_count == 0 ||
        core->state != IAP2_CORE_READY) {
        return false;
    }
    pending_csm_t pending = core->pending[core->pending_head];
    core->pending_head = (core->pending_head + 1U) % IAP2_PENDING_CSM_DEPTH;
    --core->pending_count;
    return iap2_core_send_csm_with_ack_callback(core, pending.message_id, pending.body,
                                                pending.body_len, pending.acked_callback,
                                                pending.acked_context, now_ms);
}

iap2_core_state_t iap2_core_state(const iap2_core_t *core)
{
    return core != NULL ? core->state : IAP2_CORE_DEAD;
}

uint32_t iap2_core_rx_frames(const iap2_core_t *core)
{
    return core != NULL ? core->rx_frames : 0;
}

uint32_t iap2_core_bad_frames(const iap2_core_t *core)
{
    return core != NULL ? core->bad_frames : 0;
}
