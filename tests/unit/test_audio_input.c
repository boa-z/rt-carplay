/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtcp_linux.h"
#include "mbedtls/chachapoly.h"
#include "../../examples/rtthread_app/audio_input_lubanlite.c"
extern int host_rtthread_fail_start;
void host_rtthread_join(void);
struct rtcp_audio_capture { unsigned phase; };
static struct host_audio_device mic_device;
static atomic_uint source_opens,source_closes;
rt_device_t rt_device_find(const char *name) { assert(!strcmp(name,"dmic0"));return &mic_device; }
int rtcp_audio_capture_open(rtcp_audio_capture_t **out) { *out=calloc(1,sizeof(**out));++source_opens;return 0; }
int rtcp_audio_capture_read(rtcp_audio_capture_t *c,int16_t *pcm,size_t frames) {
 assert(frames==480);for(unsigned i=0;i<480;++i) {int16_t s=(int16_t)((c->phase++%48)*100-2400);pcm[2*i]=pcm[2*i+1]=s;}
 rt_thread_mdelay(10);return 480;
}
void rtcp_audio_capture_close(rtcp_audio_capture_t *c){if(c){++source_closes;free(c);}}
static void run_input(uint64_t bits)
{
 int fd=rtcp_net_linux.udp_open(RTCP_AF_INET,0,false);assert(fd>=0);
 airplay_audio_input_config_t config={.net=&rtcp_net_linux};
 config.peer=(rtcp_addr_t){.family=RTCP_AF_INET,.addr={127,0,0,1},.port=rtcp_net_linux.local_port(fd)};
 config.sender.payload_type=100;memset(config.sender.input_key,0x55,32);
 assert(rtcp_audio_format_parse(bits,&config.sender.format));
 assert(carplay_audio_input_start(&config));
 uint8_t wire[1500],data[1500];rtcp_addr_t source;
 mbedtls_chachapoly_context cipher;mbedtls_chachapoly_init(&cipher);
 assert(mbedtls_chachapoly_setkey(&cipher,config.sender.input_key)==0);
 for(unsigned i=0;i<4;++i) {
  struct rtcp_pollfd p={.fd=fd,.events=RTCP_POLLIN};assert(rtcp_net_linux.poll(&p,1,2000)>0);
  int n=rtcp_net_linux.udp_recvfrom(fd,wire,sizeof(wire),&source);assert(n>36 && n<=1452);
  uint8_t nonce[12]={0};memcpy(nonce+4,wire+n-8,8);assert(nonce[4]==i);
  assert(wire[0]==0x80 && wire[1]==100 && wire[3]==i);
  assert(mbedtls_chachapoly_auth_decrypt(&cipher,(size_t)n-36,nonce,wire+4,8,wire+n-24,wire+12,data)==0);
  if(bits==0x4000)assert(n==996); /* 10 ms at 48k mono */
 }
 carplay_audio_input_mute(true);carplay_audio_input_request_stop();
 carplay_audio_input_stop();host_rtthread_join();
 assert(!atomic_load(&input.alive));
 mbedtls_chachapoly_free(&cipher);rtcp_net_linux.close(fd);
}
int main(void)
{
 run_input(0x4000);
#ifdef RTCP_AUDIO_CODECS
 run_input(0x20000000);
#endif
 assert(source_opens==source_closes);
 puts("actual microphone worker: virtual capture -> voice DSP -> PCM/Opus -> authenticated UDP, stop/restart passed");
}
