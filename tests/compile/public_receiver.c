/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Compile with only -Iinclude: a host composition must not need private
 * protocol, GUI, OS, SoC or codec headers to name its public configuration. */
#include "rtcp/receiver.h"
#include "rtcp/receiver_compat.h"
#include "rtcp/input.h"
#include "rtcp/display.h"
#include "rtcp/opack.h"
#include "rtcp/vehicle.h"
#include "rtcp/data_stream.h"
#include "rtcp/ui_sync.h"

int public_receiver_host(const rtcp_platform_t *platform,
                         const rtcp_media_backend_t *media,
                         const rtcp_iap2_backend_t *iap2,
                         rtcp_receiver_config_t *config,
                         rtcp_receiver_t **receiver)
{
    config->platform = *platform;
    config->media = *media;
    config->iap2 = *iap2;
    return rtcp_receiver_create(config, receiver);
}

int public_receiver_diagnostics(rtcp_receiver_t *receiver,
                                 rtcp_receiver_diagnostics_t *diagnostics,
                                 rtcp_addr_t *peer)
{
    int result = rtcp_receiver_get_diagnostics(receiver, diagnostics);
    if (result != RTCP_OK) return result;
    return rtcp_receiver_get_peer(receiver, peer);
}

int public_receiver_data_stream(rtcp_receiver_t *receiver,
    rtcp_data_stream_status_t *status, const uint8_t *body, size_t length)
{
    int result = rtcp_receiver_get_data_stream_status(receiver, RTCP_DATA_STREAM_VEHICLE, status);
    if (result) return result;
    return rtcp_receiver_send_data_stream(receiver, RTCP_DATA_STREAM_VEHICLE, body, length);
}

int public_receiver_vehicle(rtcp_receiver_t *receiver, rtcp_vehicle_delivery_status_t *status)
{
    return rtcp_receiver_get_vehicle_delivery_status(receiver, status);
}

int public_receiver_ui_sync(rtcp_receiver_t *receiver, rtcp_ui_sync_delivery_status_t *status)
{
    return rtcp_receiver_get_ui_sync_delivery_status(receiver, status);
}
