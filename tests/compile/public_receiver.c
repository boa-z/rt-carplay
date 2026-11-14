/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Compile with only -Iinclude: a host composition must not need private
 * protocol, GUI, OS, SoC or codec headers to name its public configuration. */
#include "rtcp/receiver.h"
#include "rtcp/receiver_compat.h"
#include "rtcp/input.h"

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
