/* SPDX-License-Identifier: GPL-3.0-or-later
 * Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI); see NOTICE.
 */
#ifndef RTCP_DATA_STREAM_H
#define RTCP_DATA_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTCP_DATA_STREAM_VEHICLE_UUID      "3E2F3C61-AAD0-42CB-A8AA-BF22186DA62E"
#define RTCP_DATA_STREAM_VEHICLE_HIGH_UUID "FF4A6720-F2BE-4F56-A3E1-DB3B4E37D634"
#define RTCP_DATA_STREAM_UI_SYNC_UUID      "07D9F906-8D64-4B54-A808-A20BCA2C51C2"
typedef enum
{
    RTCP_DATA_STREAM_VEHICLE,
    RTCP_DATA_STREAM_VEHICLE_HIGH,
    RTCP_DATA_STREAM_UI_SYNC,
    RTCP_DATA_STREAM_COUNT,
} rtcp_data_stream_kind_t;
#define RTCP_DATA_STREAM_MASK(kind) (1U << (kind))
#define RTCP_DATA_STREAM_ALL_MASK   ((1U << RTCP_DATA_STREAM_COUNT) - 1U)

typedef enum
{
    RTCP_DATA_STREAM_IDLE,
    RTCP_DATA_STREAM_LISTENING,
    RTCP_DATA_STREAM_CONNECTED,
    RTCP_DATA_STREAM_CLOSED,
    RTCP_DATA_STREAM_FAILED,
} rtcp_data_stream_state_t;
typedef struct
{
    bool registered, offered;
    bool enabled; /* Accepted initial SETUP enabledFeatures, not phone execution. */
    rtcp_data_stream_state_t state;
    int error;
    uint32_t stream_id;
    uint16_t port;
    uint64_t received, delivered, ignored, sent, sync_replies, rejected_peers;
    size_t queued_packages, queued_bytes;
} rtcp_data_stream_status_t;

/* Optional transport registration, disabled when channel_mask is zero.
 * It does not advertise enabledFeatures, vehicleStateProtocolInfo, uiSyncInfo
 * or displayPanels, or implement Vehicle State/UI Sync itself. The host must
 * register only protocols it can handle. Each callback runs on the Receiver
 * owner. on_body borrows one complete authenticated comm/cmnd body and may
 * queue a response; false terminates this channel. No lifecycle reentrancy.
 * on_closed runs once for an owned endpoint, including a listener that never
 * connected; error=0 means requested retirement, not delivery confirmation. */
typedef struct
{
    uint32_t channel_mask;
    bool (*on_body)(rtcp_data_stream_kind_t, const uint8_t *, size_t, void *);
    void (*on_closed)(rtcp_data_stream_kind_t, int error, void *);
    void *context;
} rtcp_data_stream_backend_t;
#endif
