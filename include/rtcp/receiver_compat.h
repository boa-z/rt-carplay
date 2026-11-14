/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_RECEIVER_COMPAT_H
#define RTCP_RECEIVER_COMPAT_H
#include "rtcp/receiver.h"
#ifdef __cplusplus
extern "C" {
#endif
struct airplay_rtsp_server_config;
/* Migration from the pre-V2.1 low-level configuration. Copies descriptors,
 * not their pointed-to providers. Caller adds on_state/context afterwards.
 * New hosts should populate rtcp_receiver_config_t directly. */
int rtcp_receiver_config_from_airplay(const struct airplay_rtsp_server_config *,
                                      rtcp_receiver_config_t *);
#ifdef __cplusplus
}
#endif
#endif
