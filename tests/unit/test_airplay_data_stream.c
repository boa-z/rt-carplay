/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_data_stream.h"
#include "airplay_control_cipher.h"
#include "rtcp/rtcp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t input[150000], output[150000], delivered[150000];
static size_t input_length,input_offset,output_length,delivered_length;
static size_t recv_limit,send_limit;
static unsigned closed,calls;
static int send_error,recv_error,listen_error,accept_error;
static bool refuse,respond;
static airplay_data_stream_t *active;
static rtcp_addr_t incoming;
static const rtcp_addr_t expected={.family=RTCP_AF_INET,.addr={192,168,169,2}};
static const uint8_t read_key[32]={1,3,5},write_key[32]={2,4,6};
static int listener(uint8_t family,uint16_t port)
{ assert(family==incoming.family && port==0); return listen_error ? listen_error : 10; }
static uint16_t port(int fd) { assert(fd==10); return 12345; }
static int accept_peer(int fd,rtcp_addr_t *peer)
{ assert(fd==10); *peer=incoming; return accept_error ? accept_error : 11; }
static int receive(int fd,void *p,size_t cap)
{
    assert(fd==11);
    if (recv_error) { int rc=recv_error; recv_error=0; return rc; }
    size_t n=input_length-input_offset;
    if (n>cap) n=cap;
    if (n>recv_limit) n=recv_limit;
    memcpy(p,input+input_offset,n); input_offset+=n;
    return (int)n;
}
static int transmit(int fd,const void *p,size_t n)
{
    assert(fd==11);
    if (send_error) { int rc=send_error; send_error=0; return rc; }
    if (n>send_limit) n=send_limit;
    assert(output_length+n<=sizeof(output));
    memcpy(output+output_length,p,n); output_length+=n;
    return (int)n;
}
static void close_fd(int fd) { assert(fd==10 || fd==11); ++closed; }
static const struct rtcp_net net={.listen=listener,.accept=accept_peer,.local_port=port,
    .recv=receive,.send=transmit,.close=close_fd};
static bool on_body(const uint8_t *p,size_t n,void *context)
{
    assert(context==&calls);
    assert(delivered_length+n<=sizeof(delivered));
    if (n) memcpy(delivered+delivered_length,p,n);
    delivered_length+=n; ++calls;
    assert(airplay_data_stream_poll(active,0)==RTCP_ESTATE);
    if (respond) assert(airplay_data_stream_send(active,(const uint8_t *)"response",8)==0);
    return !refuse;
}
static airplay_data_stream_status_t status(void)
{
    airplay_data_stream_status_t s;
    assert(airplay_data_stream_status(active,&s)==0);
    return s;
}
static void start(bool connect)
{
    input_length=input_offset=output_length=delivered_length=0;
    calls=closed=0; recv_limit=7; send_limit=5;
    send_error=recv_error=listen_error=accept_error=0;
    refuse=respond=false; incoming=expected;
    assert(airplay_data_stream_start(&net,&expected,read_key,write_key,on_body,&calls,&active)==0);
    assert(airplay_data_stream_fd(active)==10 && airplay_data_stream_port(active)==12345);
    assert(airplay_data_stream_events(active)==RTCP_POLLIN);
    assert(status().state==AIRPLAY_DATA_STREAM_LISTENING);
    assert(airplay_data_stream_send(active,NULL,0)==RTCP_ESTATE);
    if (connect) {
        assert(airplay_data_stream_poll(active,RTCP_POLLIN)==0);
        assert(airplay_data_stream_fd(active)==11 && closed==1);
        assert(status().state==AIRPLAY_DATA_STREAM_CONNECTED);
    }
}
static airplay_control_cipher_t *phone(void)
{
    airplay_control_cipher_t *c=NULL;
    assert(airplay_control_cipher_create(write_key,read_key,&c)==AIRPLAY_CONTROL_CIPHER_OK);
    return c;
}
static void seal(airplay_control_cipher_t *c,const uint8_t *p,size_t n)
{
    size_t written=0;
    assert(airplay_control_cipher_encrypt_message(c,p,n,input+input_length,
        sizeof(input)-input_length,&written)==AIRPLAY_CONTROL_CIPHER_OK);
    input_length+=written;
}
static void put32(uint8_t *p,uint32_t v)
{ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static size_t packet(uint8_t *p,const char *kind,const char *type,const uint8_t *b,size_t n)
{
    memset(p,0,32); put32(p,(uint32_t)(n+32));
    memcpy(p+4,kind,4); memcpy(p+16,type,4);
    if (n) memcpy(p+32,b,n);
    return 32+n;
}
static int read_all(void)
{
    int rc=0;
    while (input_offset<input_length && !rc) rc=airplay_data_stream_poll(active,RTCP_POLLIN);
    return rc;
}
static void flush(void)
{
    size_t budget=50000;
    while (status().queued_packages && budget--) assert(airplay_data_stream_poll(active,RTCP_POLLOUT)==0);
    assert(!status().queued_packages && budget);
    assert(airplay_data_stream_events(active)==RTCP_POLLIN);
}
static size_t unseal(airplay_control_cipher_t *c,uint8_t *p,size_t cap)
{
    size_t used=0,n=0;
    assert(airplay_control_cipher_decrypt(c,output,output_length,&used,p,cap,&n)==AIRPLAY_CONTROL_CIPHER_OK);
    assert(used==output_length);
    return n;
}
static void stop(void) { airplay_data_stream_stop(active); active=NULL; }

static void test_duplex(void)
{
    start(false);
    incoming.addr[3]^=1;
    assert(airplay_data_stream_poll(active,RTCP_POLLIN)==0 && closed==1);
    assert(status().rejected_peers==1 && !calls);
    incoming=expected;
    assert(airplay_data_stream_poll(active,RTCP_POLLIN)==0);
    uint8_t p[5200],body[5000],plain[5300];
    for (size_t i=0;i<sizeof(body);++i) body[i]=(uint8_t)i;
    size_t n=packet(p,"sync","comm",body,sizeof(body));
    static const uint8_t token[8]={0x80,2,3,4,5,6,7,0xff};
    memcpy(p+20,token,8);
    n+=packet(p+n,"rply","\0\0\0\0",NULL,0);
    n+=packet(p+n,"asyn","zzzz",(const uint8_t *)"ignored",7);
    airplay_control_cipher_t *c=phone();
    /* Header split independently from the encrypted frame and TCP boundaries. */
    seal(c,p,13); seal(c,p+13,n-13);
    recv_error=RTCP_ETIMEDOUT;
    assert(airplay_data_stream_poll(active,RTCP_POLLIN)==0 && !calls);
    respond=true;
    assert(read_all()==0 && calls==1 && delivered_length==sizeof(body));
    assert(!memcmp(body,delivered,sizeof(body)));
    assert(status().received==3 && status().ignored==2 && status().sync_replies==1);
    assert(status().queued_packages==2);
    send_error=RTCP_ETIMEDOUT;
    assert(airplay_data_stream_poll(active,RTCP_POLLOUT)==0 && !output_length);
    flush();
    assert(unseal(c,plain,sizeof(plain))==72);
    assert(!memcmp(plain+4,"rply",4) && !memcmp(plain+20,token,8));
    assert(!memcmp(plain+32+4,"asyn",4) && !memcmp(plain+32+16,"cmnd",4));
    assert(!memcmp(plain+64,"response",8) && status().sent==2);
    airplay_control_cipher_destroy(c); stop(); assert(closed==3);
}
static void test_queue_bounds(void)
{
    start(true);
    uint8_t body[5000],plain[40000],p[32]; memset(body,0x39,sizeof(body));
    for (unsigned i=0;i<7;++i) assert(airplay_data_stream_send(active,body,sizeof(body))==0);
    assert(airplay_data_stream_send(active,body,1)==RTCP_ENOMEM);
    memset(body,0xff,sizeof(body)); /* queued bodies are copied */
    airplay_control_cipher_t *c=phone();
    seal(c,p,packet(p,"sync","comm",NULL,0));
    assert(read_all()==0 && calls==1 && status().queued_packages==8);
    assert(airplay_data_stream_send(active,NULL,0)==RTCP_ENOMEM);
    flush();
    size_t n=unseal(c,plain,sizeof(plain));
    assert(n==7*5032+32);
    for (size_t i=0;i<7;++i) {
        assert(!memcmp(plain+i*5032+16,"cmnd",4));
        for (size_t j=32;j<5032;++j) assert(plain[i*5032+j]==0x39);
    }
    assert(!memcmp(plain+7*5032+4,"rply",4));
    airplay_control_cipher_destroy(c); stop();

    start(true);
    static uint8_t huge[AIRPLAY_DATA_STREAM_MAX_BODY];
    assert(airplay_data_stream_send(active,huge,sizeof(huge))==0);
    assert(airplay_data_stream_send(active,huge,sizeof(huge))==RTCP_ENOMEM);
    assert(status().queued_packages==1);
    assert(airplay_data_stream_send(active,huge,sizeof(huge)+1)==RTCP_EINVAL);
    stop();
}
static void test_terminal_failures(void)
{
    for (unsigned mode=0;mode<7;++mode) {
        start(true);
        uint8_t p[40]; size_t n=packet(p,"asyn","comm",(const uint8_t *)"payload",7);
        airplay_control_cipher_t *c=phone();
        if (mode==0) put32(p,31);
        if (mode==1) put32(p,AIRPLAY_DATA_STREAM_MAX_BODY+33);
        if (mode==2) memcpy(p+4,"bad!",4);
        if (mode==3) { n=35; } /* authenticated but incomplete package */
        if (mode==5) refuse=true;
        seal(c,p,n);
        if (mode==4) input[input_length-1]^=1;
        if (mode==6) recv_error=RTCP_EIO;
        int rc=read_all();
        if (mode==3) { assert(!rc && !calls); rc=airplay_data_stream_poll(active,RTCP_POLLIN); }
        assert(rc<0 && calls==(mode==5 ? 1U : 0U));
        assert(status().state==AIRPLAY_DATA_STREAM_FAILED && status().error==rc);
        assert(airplay_data_stream_fd(active)==-1 && !airplay_data_stream_events(active));
        assert(airplay_data_stream_send(active,NULL,0)==RTCP_ESTATE);
        assert(airplay_data_stream_poll(active,RTCP_POLLIN)==RTCP_ESTATE);
        assert(closed==2); stop(); assert(closed==2);
        airplay_control_cipher_destroy(c);
    }
    start(true);
    assert(airplay_data_stream_send(active,(const uint8_t *)"pending",7)==0);
    send_error=RTCP_EIO;
    assert(airplay_data_stream_poll(active,RTCP_POLLOUT)==RTCP_EIO);
    assert(!status().queued_packages && !status().queued_bytes); stop();
    start(true);
    assert(airplay_data_stream_poll(active,RTCP_POLLERR)==RTCP_EIO); stop();
}
static void test_ipv6_and_arguments(void)
{
    start(false); stop();
    rtcp_addr_t addr={.family=RTCP_AF_INET6,.addr={0xfe,0x80},.scope_id=3}; incoming=addr;
    assert(airplay_data_stream_start(&net,&addr,read_key,write_key,on_body,&calls,&active)==0);
    incoming.scope_id=4;
    assert(airplay_data_stream_poll(active,RTCP_POLLIN)==0);
    incoming=addr;
    assert(airplay_data_stream_poll(active,RTCP_POLLIN)==0);
    assert(status().rejected_peers==1 && status().state==AIRPLAY_DATA_STREAM_CONNECTED); stop();
    struct rtcp_net invalid=net; invalid.send=NULL;
    assert(airplay_data_stream_start(&invalid,&expected,read_key,write_key,on_body,NULL,&active)==RTCP_EINVAL && !active);
    assert(airplay_data_stream_start(NULL,NULL,NULL,NULL,NULL,NULL,NULL)==RTCP_EINVAL);
    assert(airplay_data_stream_poll(NULL,0)==RTCP_EINVAL);
    assert(airplay_data_stream_status(NULL,NULL)==RTCP_EINVAL);
    assert(airplay_data_stream_fd(NULL)==-1 && !airplay_data_stream_events(NULL));
    airplay_data_stream_stop(NULL);
}
static void test_truncation_and_maximum(void)
{
    for (size_t cut=1;cut<39;++cut) {
        start(true);
        uint8_t p[39]; packet(p,"sync","comm",(const uint8_t *)"payload",7);
        airplay_control_cipher_t *c=phone(); seal(c,p,cut);
        assert(read_all()==0 && !calls && !status().queued_packages);
        assert(airplay_data_stream_poll(active,RTCP_POLLIN)==RTCP_EIO);
        assert(!calls); airplay_control_cipher_destroy(c); stop();
    }
    start(true); recv_limit=1200; send_limit=2000;
    static uint8_t p[AIRPLAY_DATA_STREAM_MAX_BODY+32],plain[AIRPLAY_DATA_STREAM_MAX_BODY+32];
    memset(plain,0x57,AIRPLAY_DATA_STREAM_MAX_BODY);
    size_t n=packet(p,"asyn","comm",plain,AIRPLAY_DATA_STREAM_MAX_BODY);
    airplay_control_cipher_t *c=phone(); seal(c,p,n);
    assert(read_all()==0 && calls==1 && delivered_length==AIRPLAY_DATA_STREAM_MAX_BODY);
    assert(!memcmp(delivered,plain,delivered_length));
    assert(airplay_data_stream_send(active,delivered,delivered_length)==0);
    flush(); assert(unseal(c,plain,sizeof(plain))==n);
    assert(!memcmp(plain+16,"cmnd",4) && !memcmp(plain+32,delivered,delivered_length));
    airplay_control_cipher_destroy(c); stop();

    /* A burst exceeding the reserved ACK capacity fails visibly; it neither
     * silently drops replies nor delivers an unacknowledged ninth request. */
    start(true); recv_limit=1200;
    uint8_t burst[9*32];
    for (size_t i=0;i<9;++i) packet(burst+i*32,"sync","comm",NULL,0);
    c=phone(); seal(c,burst,sizeof(burst));
    assert(read_all()==RTCP_ENOMEM && calls==8 && !status().queued_packages);
    assert(status().error==RTCP_ENOMEM && status().sync_replies==8);
    airplay_control_cipher_destroy(c); stop();
}
int main(void)
{
    test_duplex(); test_queue_bounds(); test_terminal_failures(); test_ipv6_and_arguments();
    test_truncation_and_maximum();
    puts("DataStream tests passed: authenticated complete bodies, duplex sync/token, short writes, bounded queues, peer binding, fail-closed teardown");
    return 0;
}
