/* SPDX-License-Identifier: GPL-3.0-or-later
 * Wire format cross-checked against DiPlay IapTunnel.kt:
 * NetSocketChaCha20Poly1305 -> 32-byte APTransportPackage -> iAP2 link bytes.
 */
#include "airplay_iap_tunnel.h"
#include "rtcp/rtcp.h"
#include <stdlib.h>
#include <string.h>
#include "mbedtls/platform_util.h"

struct airplay_iap_tunnel {
    const struct rtcp_net *net;
    rtcp_addr_t peer;
    int listener, client;
    uint16_t port;
    airplay_control_cipher_t *cipher;
    airplay_iap_bytes_cb_t on_bytes;
    void *context;
    uint8_t header[32];
    size_t header_len;
    uint32_t remaining;
    bool comm, failed;
    uint8_t rx[1200], plain[AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES];
};
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | p[3];
}
static bool feed(airplay_iap_tunnel_t *t, const uint8_t *p, size_t len)
{
    while (len) {
        if (!t->remaining) {
            size_t n = 32U - t->header_len;
            if (n > len) n = len;
            memcpy(t->header + t->header_len, p, n);
            t->header_len += n; p += n; len -= n;
            if (t->header_len < 32U) continue;
            uint32_t size = be32(t->header);
            if (size < 32U || size > 4U*1024U*1024U) return false;
            t->remaining = size - 32U;
            t->comm = be32(t->header + 16) == 0x636f6d6dU;
            t->header_len = 0;
            if (!t->remaining) continue;
        }
        size_t n = len < t->remaining ? len : t->remaining;
        if (t->comm && !t->on_bytes(p, n, t->context)) return false;
        p += n; len -= n; t->remaining -= (uint32_t)n;
    }
    return true;
}
int airplay_iap_tunnel_start(const struct rtcp_net *net, const rtcp_addr_t *peer,
    const uint8_t key[32], airplay_iap_bytes_cb_t cb, void *ctx, airplay_iap_tunnel_t **out)
{
    if (!out) return RTCP_EINVAL;
    *out = NULL;
    if (!net || !peer || !key || !cb) return RTCP_EINVAL;
    airplay_iap_tunnel_t *t = calloc(1, sizeof(*t));
    if (!t) return RTCP_ENOMEM;
    t->net = net; t->peer = *peer; t->listener = t->client = -1;
    t->on_bytes = cb; t->context = ctx;
    uint8_t unused[32] = {0}; /* receive-only; outbound uses event /command */
    if (airplay_control_cipher_create(key, unused, &t->cipher) != AIRPLAY_CONTROL_CIPHER_OK)
        goto fail;
    t->listener = net->listen(peer->family, 0);
    if (t->listener < 0 || !net->local_port) goto fail;
    t->port = net->local_port(t->listener);
    if (!t->port) goto fail;
    *out = t;
    return RTCP_OK;
fail:
    airplay_iap_tunnel_stop(t);
    return RTCP_EIO;
}
int airplay_iap_tunnel_fd(const airplay_iap_tunnel_t *t)
{
    return !t || t->failed ? -1 : t->client >= 0 ? t->client : t->listener;
}
uint16_t airplay_iap_tunnel_port(const airplay_iap_tunnel_t *t) { return t ? t->port : 0; }
bool airplay_iap_tunnel_poll(airplay_iap_tunnel_t *t)
{
    if (!t || t->failed) return false;
    if (t->client < 0) {
        rtcp_addr_t peer = {0};
        int fd = t->net->accept(t->listener, &peer);
        if (fd < 0) return true;
        if (peer.family != t->peer.family || memcmp(peer.addr, t->peer.addr,
                peer.family == RTCP_AF_INET ? 4U : 16U) != 0) {
            t->net->close(fd);
            return true;
        }
        t->client = fd;
        t->net->close(t->listener); t->listener = -1;
        if (!t->on_bytes(NULL, 0, t->context)) goto fail;
        return true;
    }
    int n = t->net->recv(t->client, t->rx, sizeof(t->rx));
    if (n == RTCP_ETIMEDOUT) return true;
    if (n <= 0) goto fail;
    size_t offset = 0;
    bool drain = false;
    do {
        size_t used = 0, written = 0;
        airplay_control_cipher_result_t result = airplay_control_cipher_decrypt(
            t->cipher, t->rx + offset, (size_t)n - offset, &used,
            t->plain, sizeof(t->plain), &written);
        drain = result == AIRPLAY_CONTROL_CIPHER_OUTPUT_TOO_SMALL;
        if (!drain && result != AIRPLAY_CONTROL_CIPHER_OK && result != AIRPLAY_CONTROL_CIPHER_INCOMPLETE)
            goto fail;
        if (written && !feed(t, t->plain, written)) goto fail;
        if (!used && !written) goto fail;
        offset += used;
    } while (offset < (size_t)n || drain);
    return true;
fail:
    t->failed = true;
    if (t->client >= 0) { t->net->close(t->client); t->client = -1; }
    return false;
}
void airplay_iap_tunnel_stop(airplay_iap_tunnel_t *t)
{
    if (!t) return;
    if (t->listener >= 0) t->net->close(t->listener);
    if (t->client >= 0) t->net->close(t->client);
    airplay_control_cipher_destroy(t->cipher);
    mbedtls_platform_zeroize(t, sizeof(*t));
    free(t);
}
