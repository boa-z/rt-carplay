/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_iap_tunnel.h"
#include "airplay_bplist.h"
#include "rtcp/rtcp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t wire[10000], captured[10000];
static size_t wire_len, wire_at, captured_len, recv_limit;
static unsigned opened, closed;
static rtcp_addr_t peer;
static int listen_socket(uint8_t family, uint16_t port)
{ assert(family == RTCP_AF_INET && port == 0); return 1; }
static uint16_t local_port(int fd) { assert(fd == 1); return 12345; }
static int accept_socket(int fd, rtcp_addr_t *address)
{ assert(fd == 1); *address = peer; return 2; }
static int receive_socket(int fd, void *p, size_t cap)
{
    assert(fd == 2);
    size_t n = wire_len - wire_at;
    if (n > cap) n = cap;
    if (n > recv_limit) n = recv_limit;
    memcpy(p, wire + wire_at, n); wire_at += n;
    return (int)n;
}
static void close_socket(int fd) { assert(fd == 1 || fd == 2); ++closed; }
static bool receive_iap(const uint8_t *data, size_t len, void *context)
{
    assert(context == &opened);
    if (!data && !len) { ++opened; return true; }
    assert(captured_len + len <= sizeof(captured));
    memcpy(captured + captured_len, data, len); captured_len += len;
    return true;
}
static const struct rtcp_net net = {.listen=listen_socket, .local_port=local_port,
    .accept=accept_socket, .recv=receive_socket, .close=close_socket};
static const rtcp_addr_t expected = {.family=RTCP_AF_INET, .addr={192,168,169,2}};
static const uint8_t key[32] = {1, 2, 3};
static airplay_iap_tunnel_t *start(void)
{
    wire_len=wire_at=captured_len=opened=closed=0; recv_limit=7; peer=expected;
    airplay_iap_tunnel_t *t=NULL;
    assert(airplay_iap_tunnel_start(&net, &expected, key, receive_iap, &opened, &t) == 0);
    assert(airplay_iap_tunnel_port(t) == 12345);
    assert(airplay_iap_tunnel_fd(t) == 1);
    return t;
}
static void append_frame(airplay_control_cipher_t *cipher, const uint8_t *p, size_t n)
{
    size_t used=0;
    assert(airplay_control_cipher_encrypt(cipher, p, n, wire + wire_len,
        sizeof(wire) - wire_len, &used) == AIRPLAY_CONTROL_CIPHER_OK);
    wire_len += used;
}
static void test_fragmentation_and_auth(void)
{
    airplay_iap_tunnel_t *t=start();
    /* A foreign socket must not start the iAP2 state machine or consume a nonce. */
    peer.addr[3]=99;
    assert(airplay_iap_tunnel_poll(t) && opened == 0 && closed == 1);
    peer=expected;
    assert(airplay_iap_tunnel_poll(t) && opened == 1 && closed == 2);
    uint8_t plain[5064]={0};
    /* A non-comm package, then a large comm package crossing cipher frames. */
    plain[3]=32;
    plain[32+2]=0x13; plain[32+3]=0xa8; /* 5032 bytes */
    memcpy(plain+32+16, "comm", 4);
    for (size_t i=64;i<sizeof(plain);++i) plain[i]=(uint8_t)i;
    airplay_control_cipher_t *sender=NULL;
    assert(airplay_control_cipher_create(key,key,&sender) == AIRPLAY_CONTROL_CIPHER_OK);
    append_frame(sender, plain, 13);
    append_frame(sender, plain+13, 4096);
    append_frame(sender, plain+4109, sizeof(plain)-4109);
    while (wire_at < wire_len) assert(airplay_iap_tunnel_poll(t));
    assert(captured_len == 5000 && memcmp(captured, plain+64, 5000) == 0);
    /* Next frame with a bad authentication tag never dispatches a byte. */
    append_frame(sender, plain, 32); wire[wire_len-1]^=1;
    bool ok=true;
    while (wire_at < wire_len && ok) ok=airplay_iap_tunnel_poll(t);
    assert(!ok && captured_len == 5000 && airplay_iap_tunnel_fd(t) == -1);
    airplay_control_cipher_destroy(sender);
    airplay_iap_tunnel_stop(t);
    assert(closed == 3);
}
static void test_invalid_package(void)
{
    airplay_iap_tunnel_t *t=start();
    assert(airplay_iap_tunnel_poll(t));
    uint8_t invalid[32]={0}; invalid[3]=31;
    airplay_control_cipher_t *sender=NULL;
    assert(airplay_control_cipher_create(key,key,&sender) == AIRPLAY_CONTROL_CIPHER_OK);
    append_frame(sender,invalid,sizeof(invalid));
    bool ok=true;
    while(wire_at<wire_len && ok) ok=airplay_iap_tunnel_poll(t);
    assert(!ok && !captured_len);
    airplay_control_cipher_destroy(sender);
    airplay_iap_tunnel_stop(t);
}
static void test_outbound_plist(void)
{
    uint8_t body[2000], payload[1500]; size_t len=0, n=0; const uint8_t *bytes=NULL;
    memset(payload,0xaa,sizeof(payload));
    assert(airplay_bplist_build_iap_command(payload,sizeof(payload),body,sizeof(body),&len)==AIRPLAY_BPLIST_OK);
    airplay_bplist_doc_t doc; airplay_bplist_object_t root, o;
    assert(airplay_bplist_open(body,len,&doc)==AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc,&root)==AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc,root,"type",&o)==AIRPLAY_BPLIST_OK);
    char type[32];
    assert(airplay_bplist_get_string(&doc,o,type,sizeof(type),&n)==AIRPLAY_BPLIST_OK);
    assert(strcmp(type,"iAPSendMessage")==0);
    assert(airplay_bplist_dict_get(&doc,root,"params",&o)==AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc,o,"data",&o)==AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_data(&doc,o,&bytes,&n)==AIRPLAY_BPLIST_OK);
    assert(n==sizeof(payload) && !memcmp(bytes,payload,n));
}
int main(void)
{
    test_fragmentation_and_auth(); test_invalid_package(); test_outbound_plist();
    puts("AirPlay iAP tunnel tests passed (peer binding, fragmented AEAD/AP packages, bad tag, malformed size, outbound plist)");
    return 0;
}
