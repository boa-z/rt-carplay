/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "airplay_control_cipher.h"
#include "rtcp/net.h"
typedef struct airplay_iap_tunnel airplay_iap_tunnel_t;
typedef bool (*airplay_iap_bytes_cb_t)(const uint8_t *, size_t, void *);
/* Owned and polled by the RTSP owner. A failed connection never reuses keys. */
int airplay_iap_tunnel_start(const struct rtcp_net *, const rtcp_addr_t *,
    const uint8_t key[32], airplay_iap_bytes_cb_t, void *, airplay_iap_tunnel_t **);
int airplay_iap_tunnel_fd(const airplay_iap_tunnel_t *);
uint16_t airplay_iap_tunnel_port(const airplay_iap_tunnel_t *);
bool airplay_iap_tunnel_poll(airplay_iap_tunnel_t *);
void airplay_iap_tunnel_stop(airplay_iap_tunnel_t *);
