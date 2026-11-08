/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_MEDIA_H
#define RTCP_MEDIA_H

#include <stddef.h>
#include <stdint.h>

struct rtcp_video_frame {
    const uint8_t *data;
    size_t         len;
    uint64_t       pts_us;
    uint8_t        format;   /* RTCP_VIDEO_AVCC / RTCP_VIDEO_ANNEXB */
};

#define RTCP_VIDEO_AVCC    1
#define RTCP_VIDEO_ANNEXB  2

struct rtcp_pcm {
    const uint8_t *data;
    size_t         len;
    uint64_t       pts_us;
};

struct rtcp_media {
    int (*video_frame)(const struct rtcp_video_frame *f);
    int (*audio_pcm)(const struct rtcp_pcm *p);
    int (*audio_config)(uint32_t rate, uint8_t channels, uint8_t bits);
    int (*mic_read)(void *buf, size_t len);
};

#endif /* RTCP_MEDIA_H */
