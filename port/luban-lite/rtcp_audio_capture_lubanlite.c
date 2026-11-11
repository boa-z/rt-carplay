/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtcp_audio_capture_lubanlite.h"
#include "rtcp/rtcp.h"
#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/audio.h>
#include <stdlib.h>
#include <string.h>
struct rtcp_audio_capture { rt_device_t dev; };
int rtcp_audio_capture_open(rtcp_audio_capture_t **out)
{
    if (!out) return RTCP_EINVAL;
    *out = NULL;
    rt_device_t dev = rt_device_find("dmic0");
    if (!dev) return RTCP_ENOENT;
    /* This port relies on the SDK audio pipe being single-reader. Do not
     * steal an application recorder's device or replace its callbacks. */
    if (dev->ref_count) return RTCP_ESTATE;
    rtcp_audio_capture_t *c = calloc(1,sizeof(*c));
    if (!c) return RTCP_ENOMEM;
    if (rt_device_init(dev) != RT_EOK) { free(c); return RTCP_EIO; }
    struct rt_audio_caps caps = {0};
    caps.main_type=AUDIO_TYPE_INPUT; caps.sub_type=AUDIO_DSP_PARAM;
    caps.udata.config.samplerate=48000;caps.udata.config.channels=2;caps.udata.config.samplebits=16;
    if (rt_device_control(dev,AUDIO_CTL_CONFIGURE,&caps)!=RT_EOK ||
        rt_device_open(dev,RT_DEVICE_OFLAG_RDONLY)!=RT_EOK) { free(c); return RTCP_EIO; }
    struct rt_audio_device *audio=(struct rt_audio_device *)dev;
    if (!audio->record) { (void)rt_device_close(dev);free(c);return RTCP_EIO; }
    c->dev=dev;*out=c;return RTCP_OK;
}
int rtcp_audio_capture_read(rtcp_audio_capture_t *c,int16_t *pcm,size_t max_frames)
{
    if (!c || !pcm || !max_frames || max_frames>480) return RTCP_EINVAL;
    struct rt_audio_device *audio=(struct rt_audio_device *)c->dev;
    /* Inspect the SDK pipe under its own IRQ exclusion. There is no other
     * reader. The ISR can add/discard oldest data but cannot empty the pipe,
     * so a nonzero snapshot makes the subsequent read nonblocking without
     * altering driver flags, cancelling reads or touching DMA storage. */
    rt_base_t level=rt_hw_interrupt_disable();
    size_t available=rt_ringbuffer_data_len(&audio->record->pipe.ringbuffer);
    rt_hw_interrupt_enable(level);
    available -= available%4;
    if (!available) return 0;
    size_t bytes=max_frames*4;
    if(bytes>available)bytes=available;
    size_t got=rt_device_read(c->dev,0,pcm,bytes);
    if (got>bytes || got%4) return RTCP_EIO;
    return (int)(got/4);
}
void rtcp_audio_capture_close(rtcp_audio_capture_t *c)
{ if (c) { (void)rt_device_close(c->dev);free(c); } }
