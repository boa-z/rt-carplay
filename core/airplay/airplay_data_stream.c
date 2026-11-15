/* SPDX-License-Identifier: GPL-3.0-or-later
 * Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI).
 * Copyright (C) 2025-2026 Lasse Heitgres. See NOTICE.
 * Bounded C adaptation of APTransportPackage framing in iap_tunnel.rs.
 */
#include "airplay_data_stream.h"
#include "airplay_control_cipher.h"
#include "rtcp/rtcp.h"
#include "mbedtls/platform_util.h"
#include <stdlib.h>
#include <string.h>

#define HEADER 32U
#define ASYNC 0x6173796eU
#define SYNC  0x73796e63U
#define REPLY 0x72706c79U
#define COMM  0x636f6d6dU
#define CMND  0x636d6e64U
#define CHUNK AIRPLAY_CONTROL_CIPHER_MESSAGE_CHUNK_BYTES

typedef struct {
    uint8_t *data;
    size_t length, offset;
} packet_t;
struct airplay_data_stream {
    const struct rtcp_net *net;
    rtcp_addr_t peer;
    int listener, client;
    uint16_t port;
    airplay_control_cipher_t *cipher;
    airplay_data_stream_body_cb_t deliver;
    void *context;
    airplay_data_stream_status_t status;
    uint8_t header[HEADER], *body;
    size_t header_used, body_length, body_used;
    packet_t queue[AIRPLAY_DATA_STREAM_QUEUE_PACKAGES];
    size_t queue_head;
    uint8_t sealed[CHUNK + AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES];
    uint8_t wire[1200], plain[AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES];
    size_t sealed_length, sealed_offset;
    bool in_poll;
};
static uint32_t load32(const uint8_t *p)
{
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static void store32(uint8_t *p, uint32_t value)
{
    p[0]=(uint8_t)(value>>24); p[1]=(uint8_t)(value>>16);
    p[2]=(uint8_t)(value>>8); p[3]=(uint8_t)value;
}
static void clear_bytes(uint8_t **p, size_t size)
{
    if (*p) { mbedtls_platform_zeroize(*p, size); free(*p); *p=NULL; }
}
static void release_resources(airplay_data_stream_t *s)
{
    if (s->listener >= 0) { s->net->close(s->listener); s->listener=-1; }
    if (s->client >= 0) { s->net->close(s->client); s->client=-1; }
    airplay_control_cipher_destroy(s->cipher); s->cipher=NULL;
    clear_bytes(&s->body, s->body_length);
    for (size_t i=0; i<AIRPLAY_DATA_STREAM_QUEUE_PACKAGES; ++i)
        clear_bytes(&s->queue[i].data, s->queue[i].length);
    s->status.queued_packages=s->status.queued_bytes=0;
    mbedtls_platform_zeroize(s->sealed, sizeof(s->sealed));
    mbedtls_platform_zeroize(s->plain, sizeof(s->plain));
    mbedtls_platform_zeroize(s->wire, sizeof(s->wire));
    mbedtls_platform_zeroize(s->header, sizeof(s->header));
}
static int fail(airplay_data_stream_t *s, int error)
{
    s->status.state=AIRPLAY_DATA_STREAM_FAILED; s->status.error=error;
    release_resources(s);
    return error;
}
static int enqueue(airplay_data_stream_t *s, uint32_t kind,
    uint32_t type, const uint8_t *token, const uint8_t *body, size_t length)
{
    size_t size=HEADER+length;
    size_t limit=AIRPLAY_DATA_STREAM_QUEUE_PACKAGES-(kind==REPLY ? 0U : 1U);
    /* Reserve both a package and enough bytes for a transport reply. */
    size_t byte_limit=AIRPLAY_DATA_STREAM_QUEUE_BYTES-(kind==REPLY ? 0U : HEADER);
    if (s->status.queued_packages >= limit || s->status.queued_bytes>byte_limit ||
        size > byte_limit-s->status.queued_bytes)
        return RTCP_ENOMEM;
    uint8_t *p=calloc(1,size);
    if (!p) return RTCP_ENOMEM;
    store32(p,(uint32_t)size); store32(p+4,kind); store32(p+16,type);
    if (token) memcpy(p+20,token,8);
    if (length) memcpy(p+HEADER,body,length);
    size_t index=(s->queue_head+s->status.queued_packages)%AIRPLAY_DATA_STREAM_QUEUE_PACKAGES;
    s->queue[index]=(packet_t){p,size,0};
    ++s->status.queued_packages; s->status.queued_bytes+=size;
    return RTCP_OK;
}
static int complete(airplay_data_stream_t *s)
{
    uint32_t kind=load32(s->header+4), type=load32(s->header+16);
    ++s->status.received;
    if (kind==SYNC) {
        int rc=enqueue(s,REPLY,0,s->header+20,NULL,0);
        if (rc) return rc;
        ++s->status.sync_replies;
    }
    if (kind!=REPLY && (type==COMM || type==CMND)) {
        if (!s->deliver(s->body,s->body_length,s->context)) return RTCP_EIO;
        ++s->status.delivered;
    } else ++s->status.ignored;
    clear_bytes(&s->body,s->body_length);
    s->header_used=s->body_length=s->body_used=0;
    return RTCP_OK;
}
static int feed(airplay_data_stream_t *s, const uint8_t *p, size_t length)
{
    while (length) {
        if (s->header_used<HEADER) {
            size_t n=HEADER-s->header_used;
            if (n>length) n=length;
            memcpy(s->header+s->header_used,p,n);
            s->header_used+=n; p+=n; length-=n;
            if (s->header_used<HEADER) continue;
            uint32_t size=load32(s->header), kind=load32(s->header+4);
            if (size<HEADER || size-HEADER>AIRPLAY_DATA_STREAM_MAX_BODY ||
                (kind!=ASYNC && kind!=SYNC && kind!=REPLY)) return RTCP_EINVAL;
            s->body_length=size-HEADER;
            if (s->body_length) {
                s->body=malloc(s->body_length);
                if (!s->body) return RTCP_ENOMEM;
            } else {
                int rc=complete(s);
                if (rc) return rc;
                continue;
            }
        }
        size_t n=s->body_length-s->body_used;
        if (n>length) n=length;
        memcpy(s->body+s->body_used,p,n);
        s->body_used+=n; p+=n; length-=n;
        if (s->body_used==s->body_length) {
            int rc=complete(s);
            if (rc) return rc;
        }
    }
    return RTCP_OK;
}
static int read_ready(airplay_data_stream_t *s)
{
    int n=s->net->recv(s->client,s->wire,sizeof(s->wire));
    if (n==RTCP_ETIMEDOUT) return RTCP_OK;
    if (n<=0 || (size_t)n>sizeof(s->wire)) return RTCP_EIO;
    size_t offset=0; bool drain;
    do {
        size_t used=0,written=0;
        airplay_control_cipher_result_t rc=airplay_control_cipher_decrypt(s->cipher,
            s->wire+offset,(size_t)n-offset,&used,s->plain,sizeof(s->plain),&written);
        drain=rc==AIRPLAY_CONTROL_CIPHER_OUTPUT_TOO_SMALL;
        if (!drain && rc!=AIRPLAY_CONTROL_CIPHER_OK && rc!=AIRPLAY_CONTROL_CIPHER_INCOMPLETE) {
            mbedtls_platform_zeroize(s->plain,sizeof(s->plain));
            return RTCP_EIO;
        }
        int result=feed(s,s->plain,written);
        mbedtls_platform_zeroize(s->plain,sizeof(s->plain));
        if (result) return result;
        if (!used && !written) return RTCP_EIO;
        offset+=used;
    } while (offset<(size_t)n || drain);
    return RTCP_OK;
}
static int write_ready(airplay_data_stream_t *s)
{
    if (!s->status.queued_packages) return RTCP_OK;
    packet_t *p=&s->queue[s->queue_head];
    if (!s->sealed_length) {
        size_t n=p->length-p->offset;
        if (n>CHUNK) n=CHUNK;
        if (airplay_control_cipher_encrypt(s->cipher,p->data+p->offset,n,
            s->sealed,sizeof(s->sealed),&s->sealed_length)!=AIRPLAY_CONTROL_CIPHER_OK)
            return RTCP_EIO;
        p->offset+=n;
    }
    size_t left=s->sealed_length-s->sealed_offset;
    int n=s->net->send(s->client,s->sealed+s->sealed_offset,left);
    if (n==RTCP_ETIMEDOUT) return RTCP_OK;
    if (n<=0 || (size_t)n>left) return RTCP_EIO;
    s->sealed_offset+=(size_t)n;
    if (s->sealed_offset==s->sealed_length) {
        mbedtls_platform_zeroize(s->sealed,s->sealed_length);
        s->sealed_offset=s->sealed_length=0;
        if (p->offset==p->length) {
            s->status.queued_bytes-=p->length;
            clear_bytes(&p->data,p->length); p->length=p->offset=0;
            s->queue_head=(s->queue_head+1U)%AIRPLAY_DATA_STREAM_QUEUE_PACKAGES;
            --s->status.queued_packages; ++s->status.sent;
        }
    }
    return RTCP_OK;
}
int airplay_data_stream_start(const struct rtcp_net *net, const rtcp_addr_t *peer,
    const uint8_t read_key[32], const uint8_t write_key[32],
    airplay_data_stream_body_cb_t cb, void *context, airplay_data_stream_t **out)
{
    if (!out) return RTCP_EINVAL;
    *out=NULL;
    if (!net || !net->listen || !net->accept || !net->recv || !net->send ||
        !net->close || !net->local_port || !peer || !read_key || !write_key || !cb ||
        (peer->family!=RTCP_AF_INET && peer->family!=RTCP_AF_INET6)) return RTCP_EINVAL;
    airplay_data_stream_t *s=calloc(1,sizeof(*s));
    if (!s) return RTCP_ENOMEM;
    s->net=net; s->peer=*peer; s->listener=s->client=-1;
    s->deliver=cb; s->context=context;
    if (airplay_control_cipher_create(read_key,write_key,&s->cipher)!=AIRPLAY_CONTROL_CIPHER_OK)
        goto error;
    s->listener=net->listen(peer->family,0);
    if (s->listener<0 || !(s->port=net->local_port(s->listener))) goto error;
    *out=s;
    return RTCP_OK;
error:
    airplay_data_stream_stop(s);
    return RTCP_EIO;
}
int airplay_data_stream_fd(const airplay_data_stream_t *s)
{
    if (!s || s->status.state==AIRPLAY_DATA_STREAM_FAILED) return -1;
    return s->client>=0 ? s->client : s->listener;
}
int airplay_data_stream_events(const airplay_data_stream_t *s)
{
    if (airplay_data_stream_fd(s)<0) return 0;
    return RTCP_POLLIN | (s->status.queued_packages ? RTCP_POLLOUT : 0);
}
uint16_t airplay_data_stream_port(const airplay_data_stream_t *s) { return s ? s->port : 0; }
int airplay_data_stream_poll(airplay_data_stream_t *s, int events)
{
    if (!s) return RTCP_EINVAL;
    if (s->status.state==AIRPLAY_DATA_STREAM_FAILED || s->in_poll) return RTCP_ESTATE;
    if (events&RTCP_POLLERR) return fail(s,RTCP_EIO);
    if (s->client<0) {
        if (!(events&RTCP_POLLIN)) return RTCP_OK;
        rtcp_addr_t peer={0};
        int fd=s->net->accept(s->listener,&peer);
        if (fd==RTCP_ETIMEDOUT) return RTCP_OK;
        if (fd<0) return fail(s,RTCP_EIO);
        if (peer.family!=s->peer.family ||
            memcmp(peer.addr,s->peer.addr,peer.family==RTCP_AF_INET ? 4U : 16U) ||
            (peer.family==RTCP_AF_INET6 && peer.scope_id!=s->peer.scope_id)) {
            s->net->close(fd); ++s->status.rejected_peers;
            return RTCP_OK;
        }
        s->client=fd; s->net->close(s->listener); s->listener=-1;
        s->status.state=AIRPLAY_DATA_STREAM_CONNECTED;
        return RTCP_OK;
    }
    s->in_poll=true;
    int rc=RTCP_OK;
    if (events&RTCP_POLLOUT) rc=write_ready(s);
    if (!rc && (events&RTCP_POLLIN)) rc=read_ready(s);
    s->in_poll=false;
    return rc ? fail(s,rc) : RTCP_OK;
}
int airplay_data_stream_send(airplay_data_stream_t *s, const uint8_t *p, size_t n)
{
    if (!s || (!p && n) || n>AIRPLAY_DATA_STREAM_MAX_BODY) return RTCP_EINVAL;
    if (s->status.state!=AIRPLAY_DATA_STREAM_CONNECTED) return RTCP_ESTATE;
    return enqueue(s,ASYNC,CMND,NULL,p,n);
}
int airplay_data_stream_status(const airplay_data_stream_t *s, airplay_data_stream_status_t *out)
{
    if (!s || !out) return RTCP_EINVAL;
    *out=s->status;
    return RTCP_OK;
}
void airplay_data_stream_stop(airplay_data_stream_t *s)
{
    if (!s) return;
    release_resources(s);
    mbedtls_platform_zeroize(s,sizeof(*s)); free(s);
}
