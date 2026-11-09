/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Luban-Lite video sink: MPP H.264 decoder -> DE video layer.
 *
 * The AirPlay poll thread calls carplay_video_sink_frame() with Annex-B
 * access units.  This file splits them into NAL units and copies them into a
 * small FIFO.  A dedicated thread feeds one NAL per decoder packet (the SDK
 * H.264 decoder walks Annex-B start codes inside a packet), pumps the
 * decoder and presents the resulting NV12 frames through the "aicfb" video
 * layer.  Only the sink thread touches the decoder and the display.
 *
 * A stream configuration message (SPS/PPS) that arrives after frames have
 * been shown marks the start of a new stream (reconnect or resolution
 * change); the decoder is recreated before that configuration is fed so the
 * new SPS can allocate buffers of the right size.
 */
#include "video_sink_lubanlite.h"

#include <rtthread.h>
#include <rtdevice.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "artinchip_fb.h"
#include "mpp_decoder.h"
#include "rtcp/rtcp.h"

#define TAG "carplay-vid"

#define SINK_QUEUE_LENGTH 16U
#define SINK_MAX_NAL_BYTES (512U * 1024U)
#define SINK_THREAD_STACK_BYTES (16U * 1024U)
#define SINK_THREAD_PRIORITY 20U
#define SINK_THREAD_TICK 20U
#define SINK_BITSTREAM_BYTES (1024U * 1024U)
#define SINK_PACKET_COUNT 2
#define SINK_EXTRA_FRAMES 1
#define SINK_MAX_PACKET_RETRIES 2000U

typedef struct {
    uint8_t *data;
    size_t len;
    uint32_t pts_us;
    bool configure;
} nal_entry_t;

typedef struct {
    struct mpp_decoder *decoder;
    rt_device_t fb;
    rt_thread_t thread;
    rt_sem_t wake;
    rt_mutex_t lock;

    uint16_t display_width;
    uint16_t display_height;
    /* Panel geometry from aicfb; the DE layer works in these coordinates. */
    uint16_t panel_width;
    uint16_t panel_height;

    nal_entry_t queue[SINK_QUEUE_LENGTH];
    unsigned head;
    unsigned tail;

    struct mpp_frame held;
    bool have_frame;
    struct aicfb_alpha_config saved_ui_alpha;
    bool saved_ui_alpha_valid;

    volatile bool stop;
    volatile bool running;
    volatile bool thread_running;

    unsigned nal_count;
    unsigned frame_count;
    unsigned dropped_nals;
    unsigned decode_errors;
    unsigned decoder_resets;
} video_sink_t;

static video_sink_t s_sink;

/* ------------------------------------------------------------------ */
/* NAL queue                                                           */
/* ------------------------------------------------------------------ */

static const uint8_t *find_start_code(const uint8_t *cursor, const uint8_t *end)
{
    while (cursor + 2 < end) {
        if (cursor[0] == 0x00 && cursor[1] == 0x00 && cursor[2] == 0x01) {
            return cursor;
        }
        ++cursor;
    }
    return NULL;
}

static void drop_nal(video_sink_t *sink)
{
    ++sink->dropped_nals;
    if (sink->dropped_nals == 1U || sink->dropped_nals % 60U == 0U) {
        rt_kprintf("[" TAG "] queue full/alloc failure, dropped=%u\n",
                   sink->dropped_nals);
    }
}

static void enqueue_nal(video_sink_t *sink, const uint8_t *data, size_t len,
                        uint32_t pts_us, bool configure)
{
    if (len == 0U || len > SINK_MAX_NAL_BYTES) {
        drop_nal(sink);
        return;
    }
    uint8_t *copy = malloc(len);
    if (copy == NULL) {
        drop_nal(sink);
        return;
    }
    memcpy(copy, data, len);

    if (rt_mutex_take(sink->lock, RT_WAITING_FOREVER) != RT_EOK) {
        free(copy);
        return;
    }
    unsigned next = (sink->head + 1U) % SINK_QUEUE_LENGTH;
    if (next == sink->tail) {
        (void)rt_mutex_release(sink->lock);
        free(copy);
        drop_nal(sink);
        return;
    }
    sink->queue[sink->head].data = copy;
    sink->queue[sink->head].len = len;
    sink->queue[sink->head].pts_us = pts_us;
    sink->queue[sink->head].configure = configure;
    sink->head = next;
    (void)rt_mutex_release(sink->lock);
}

static bool dequeue_nal(video_sink_t *sink, nal_entry_t *out)
{
    if (rt_mutex_take(sink->lock, RT_WAITING_FOREVER) != RT_EOK) return false;
    bool available = sink->tail != sink->head;
    if (available) {
        *out = sink->queue[sink->tail];
        sink->tail = (sink->tail + 1U) % SINK_QUEUE_LENGTH;
    }
    (void)rt_mutex_release(sink->lock);
    return available;
}

void carplay_video_sink_frame(bool configuration, const uint8_t *annex_b,
                              size_t annex_b_len, uint64_t sender_time_ns,
                              void *context)
{
    (void)context;
    video_sink_t *sink = &s_sink;
    if (!sink->running || annex_b == NULL || annex_b_len == 0U) return;

    uint32_t pts_us = (uint32_t)(sender_time_ns / 1000U);
    const uint8_t *end = annex_b + annex_b_len;
    const uint8_t *cursor = find_start_code(annex_b, end);
    if (cursor == NULL) return; /* not Annex-B; nothing to feed the decoder */

    while (cursor < end) {
        const uint8_t *next = find_start_code(cursor + 3U, end);
        size_t nal_len = (size_t)((next != NULL ? next : end) - cursor);
        enqueue_nal(sink, cursor, nal_len, pts_us, configuration);
        if (next == NULL) break;
        cursor = next;
    }
    (void)rt_sem_release(sink->wake);
}

/* ------------------------------------------------------------------ */
/* Decode and display                                                  */
/* ------------------------------------------------------------------ */

static void disable_video_layer(video_sink_t *sink)
{
    struct aicfb_layer_data layer;
    memset(&layer, 0, sizeof(layer));
    layer.layer_id = AICFB_LAYER_TYPE_VIDEO;
    layer.enable = 0;
    (void)rt_device_control(sink->fb, AICFB_UPDATE_LAYER_CONFIG, &layer);
}

static void present_frame(video_sink_t *sink, const struct mpp_frame *frame)
{
    struct aicfb_layer_data layer;
    memset(&layer, 0, sizeof(layer));
    layer.layer_id = AICFB_LAYER_TYPE_VIDEO;
    layer.enable = 1;

    /* The decoder asks the DE to crop H.264 macroblock padding away; the
     * scaler sees the cropped rectangle as its input. */
    int src_w = frame->buf.size.width;
    int src_h = frame->buf.size.height;
    if (frame->buf.crop_en && frame->buf.crop.width > 0 &&
        frame->buf.crop.height > 0) {
        src_w = frame->buf.crop.width;
        src_h = frame->buf.crop.height;
    }

    /* Letterbox: keep the source aspect ratio inside the panel.  The DE layer
     * position/scale is in panel pixels, not in the AirPlay display mode, so
     * use the aicfb geometry (identical when the mode matches the panel). */
    int scale_w = sink->panel_width;
    int scale_h = sink->panel_height;
    if (src_w > 0 && src_h > 0) {
        int by_width = (int)sink->panel_width * src_h / src_w;
        if (by_width <= (int)sink->panel_height) {
            scale_h = by_width;
        } else {
            scale_w = (int)sink->panel_height * src_w / src_h;
        }
    }
    layer.scale_size.width = scale_w;
    layer.scale_size.height = scale_h;
    layer.pos.x = ((int)sink->panel_width - scale_w) / 2;
    layer.pos.y = ((int)sink->panel_height - scale_h) / 2;
    memcpy(&layer.buf, &frame->buf, sizeof(layer.buf));

    if (rt_device_control(sink->fb, AICFB_UPDATE_LAYER_CONFIG, &layer) < 0) {
        ++sink->decode_errors;
        return;
    }
    (void)rt_device_control(sink->fb, AICFB_WAIT_FOR_VSYNC, &layer);
}

static unsigned drain_frames(video_sink_t *sink)
{
    unsigned presented = 0U;
    while (presented < 8U && !sink->stop && sink->decoder != NULL) {
        struct mpp_frame frame;
        memset(&frame, 0, sizeof(frame));
        int ret = mpp_decoder_get_frame(sink->decoder, &frame);
        if (ret != DEC_OK) {
            if (ret != DEC_NO_RENDER_FRAME && ret != DEC_NO_EMPTY_FRAME &&
                ret != DEC_ERR_FM_NOT_CREATE) {
                ++sink->decode_errors;
            }
            break;
        }
        present_frame(sink, &frame);
        ++presented;
        ++sink->frame_count;
        /* Return the frame that was on screen until this one; the DE is
         * done with it after the vsync wait above. */
        if (sink->have_frame) {
            (void)mpp_decoder_put_frame(sink->decoder, &sink->held);
        }
        sink->held = frame;
        sink->have_frame = true;
    }
    return presented;
}

static int submit_nal(video_sink_t *sink, const nal_entry_t *nal)
{
    if (sink->decoder == NULL) return -1;

    struct mpp_packet packet;
    memset(&packet, 0, sizeof(packet));
    int ret = -1;
    for (unsigned attempt = 0; attempt < SINK_MAX_PACKET_RETRIES; ++attempt) {
        ret = mpp_decoder_get_packet(sink->decoder, &packet, (int)nal->len);
        if (ret == DEC_OK) break;
        if (sink->stop) return -1;
        if (ret != DEC_NO_EMPTY_PACKET && ret != -1) {
            ++sink->decode_errors;
            return -1;
        }
        rt_thread_mdelay(1);
    }
    if (ret != DEC_OK) {
        ++sink->decode_errors;
        return -1;
    }
    memcpy(packet.data, nal->data, nal->len);
    packet.size = (int)nal->len;
    packet.pts = (long long)nal->pts_us;
    ret = mpp_decoder_put_packet(sink->decoder, &packet);
    if (ret != 0) {
        ++sink->decode_errors;
        return -1;
    }
    ++sink->nal_count;
    return 0;
}

static void pump_decoder(video_sink_t *sink)
{
    if (sink->decoder == NULL) return;
    for (int i = 0; i < 32 && !sink->stop; ++i) {
        int ret = mpp_decoder_decode(sink->decoder);
        if (ret == DEC_NO_READY_PACKET) break;
        if (ret == DEC_OK) continue;
        if (ret == DEC_NO_EMPTY_FRAME) {
            if (drain_frames(sink) == 0U) break;
            continue;
        }
        if (ret != 0) {
            if (ret != DEC_NO_RENDER_FRAME) {
                ++sink->decode_errors;
                if (sink->decode_errors <= 3U) {
                    rt_kprintf("[" TAG "] decode error %d\n", ret);
                }
            }
            break;
        }
    }
    (void)drain_frames(sink);
}

/* ------------------------------------------------------------------ */
/* Decoder lifetime                                                    */
/* ------------------------------------------------------------------ */

static int create_decoder(video_sink_t *sink)
{
    sink->decoder = mpp_decoder_create(MPP_CODEC_VIDEO_DECODER_H264);
    if (sink->decoder == NULL) {
        rt_kprintf("[" TAG "] cannot create the H.264 decoder\n");
        return RTCP_ENOMEM;
    }
    struct decode_config config;
    memset(&config, 0, sizeof(config));
    config.pix_fmt = MPP_FMT_NV12;
    config.bitstream_buffer_size = SINK_BITSTREAM_BYTES;
    config.packet_count = SINK_PACKET_COUNT;
    config.extra_frame_num = SINK_EXTRA_FRAMES;
    int ret = mpp_decoder_init(sink->decoder, &config);
    if (ret != 0) {
        rt_kprintf("[" TAG "] decoder init failed: %d\n", ret);
        mpp_decoder_destory(sink->decoder);
        sink->decoder = NULL;
        return RTCP_EIO;
    }
    return RTCP_OK;
}

/* Called from the sink thread only. */
static void recreate_decoder(video_sink_t *sink)
{
    if (sink->have_frame) {
        if (sink->decoder != NULL) {
            (void)mpp_decoder_put_frame(sink->decoder, &sink->held);
        }
        sink->have_frame = false;
    }
    /* Stop the DE from scanning the buffer we are about to free. */
    disable_video_layer(sink);
    mpp_decoder_destory(sink->decoder);
    sink->decoder = NULL;
    ++sink->decoder_resets;
    if (create_decoder(sink) != RTCP_OK) {
        ++sink->decode_errors;
        rt_kprintf("[" TAG "] decoder reset failed\n");
    } else {
        rt_kprintf("[" TAG "] decoder reset for a new stream\n");
    }
}

static void sink_thread(void *argument)
{
    video_sink_t *sink = argument;
    sink->thread_running = true;
    rt_kprintf("[" TAG "] decoder running\n");

    while (!sink->stop) {
        if (rt_sem_take(sink->wake, 10) == RT_EOK) {
            nal_entry_t nal;
            while (dequeue_nal(sink, &nal)) {
                if (sink->stop) {
                    free(nal.data);
                    break;
                }
                if (nal.configure && sink->frame_count != 0U) {
                    recreate_decoder(sink);
                }
                (void)submit_nal(sink, &nal);
                free(nal.data);
            }
        }
        pump_decoder(sink);
    }

    if (sink->have_frame) {
        if (sink->decoder != NULL) {
            (void)mpp_decoder_put_frame(sink->decoder, &sink->held);
        }
        sink->have_frame = false;
    }
    rt_kprintf("[" TAG "] stopped: %u NALs, %u frames, %u dropped, %u errors, %u resets\n",
               sink->nal_count, sink->frame_count, sink->dropped_nals,
               sink->decode_errors, sink->decoder_resets);
    sink->thread_running = false;
}

/* ------------------------------------------------------------------ */
/* Start / stop                                                        */
/* ------------------------------------------------------------------ */

static void set_ui_alpha(video_sink_t *sink, const struct aicfb_alpha_config *cfg)
{
    struct aicfb_alpha_config alpha = *cfg;
    alpha.layer_id = AICFB_LAYER_TYPE_UI;
    (void)rt_device_control(sink->fb, AICFB_UPDATE_ALPHA_CONFIG, &alpha);
}

int carplay_video_sink_start(uint16_t display_width, uint16_t display_height)
{
    video_sink_t *sink = &s_sink;
    if (sink->running) return RTCP_ESTATE;
    if (display_width == 0U || display_height == 0U) return RTCP_EINVAL;

    memset(sink, 0, sizeof(*sink));
    sink->display_width = display_width;
    sink->display_height = display_height;
    sink->panel_width = display_width;
    sink->panel_height = display_height;

    sink->fb = rt_device_find("aicfb");
    if (sink->fb == NULL) {
        rt_kprintf("[" TAG "] aicfb device not found\n");
        return RTCP_ENOENT;
    }

    struct aicfb_screeninfo screen;
    memset(&screen, 0, sizeof(screen));
    if (rt_device_control(sink->fb, AICFB_GET_SCREENINFO, &screen) >= 0 &&
        screen.width != 0U && screen.height != 0U) {
        sink->panel_width = (uint16_t)screen.width;
        sink->panel_height = (uint16_t)screen.height;
        rt_kprintf("[" TAG "] panel %ux%u, AirPlay display %ux%u\n",
                   (unsigned)sink->panel_width, (unsigned)sink->panel_height,
                   (unsigned)display_width, (unsigned)display_height);
    } else {
        rt_kprintf("[" TAG "] no panel info, assuming %ux%u\n",
                   (unsigned)display_width, (unsigned)display_height);
    }

    int ret = create_decoder(sink);
    if (ret != RTCP_OK) return ret;

    sink->wake = rt_sem_create("cvid", 0, RT_IPC_FLAG_FIFO);
    sink->lock = rt_mutex_create("cvidq", RT_IPC_FLAG_FIFO);
    if (sink->wake == NULL || sink->lock == NULL) {
        if (sink->wake != NULL) (void)rt_sem_delete(sink->wake);
        if (sink->lock != NULL) (void)rt_mutex_delete(sink->lock);
        sink->wake = NULL;
        sink->lock = NULL;
        mpp_decoder_destory(sink->decoder);
        sink->decoder = NULL;
        return RTCP_ENOMEM;
    }

    /* Hide the UI layer so the video layer is visible, remembering the old
     * setting for stop(). */
    struct aicfb_alpha_config ui;
    memset(&ui, 0, sizeof(ui));
    ui.layer_id = AICFB_LAYER_TYPE_UI;
    if (rt_device_control(sink->fb, AICFB_GET_ALPHA_CONFIG, &ui) >= 0) {
        sink->saved_ui_alpha = ui;
        sink->saved_ui_alpha_valid = true;
    }
    struct aicfb_alpha_config transparent;
    memset(&transparent, 0, sizeof(transparent));
    transparent.layer_id = AICFB_LAYER_TYPE_UI;
    transparent.enable = 1;
    transparent.mode = AICFB_GLOBAL_ALPHA_MODE;
    transparent.value = 0;
    set_ui_alpha(sink, &transparent);

    sink->stop = false;
    sink->running = true;
    sink->thread = rt_thread_create("carplay-vid", sink_thread, sink,
                                    SINK_THREAD_STACK_BYTES,
                                    SINK_THREAD_PRIORITY, SINK_THREAD_TICK);
    if (sink->thread == NULL) {
        sink->running = false;
        mpp_decoder_destory(sink->decoder);
        sink->decoder = NULL;
        (void)rt_sem_delete(sink->wake);
        (void)rt_mutex_delete(sink->lock);
        sink->wake = NULL;
        sink->lock = NULL;
        if (sink->saved_ui_alpha_valid) {
            set_ui_alpha(sink, &sink->saved_ui_alpha);
        }
        return RTCP_ENOMEM;
    }
    (void)rt_thread_startup(sink->thread);
    return RTCP_OK;
}

void carplay_video_sink_stop(void)
{
    video_sink_t *sink = &s_sink;
    if (!sink->running) {
        return;
    }
    sink->running = false; /* stop accepting new NALs */
    sink->stop = true;
    if (sink->wake != NULL) (void)rt_sem_release(sink->wake);
    while (sink->thread_running) {
        rt_thread_mdelay(10);
    }

    while (sink->tail != sink->head) {
        free(sink->queue[sink->tail].data);
        sink->tail = (sink->tail + 1U) % SINK_QUEUE_LENGTH;
    }

    if (sink->decoder != NULL) {
        mpp_decoder_destory(sink->decoder);
        sink->decoder = NULL;
    }

    if (sink->fb != NULL) {
        disable_video_layer(sink);
        if (sink->saved_ui_alpha_valid) {
            set_ui_alpha(sink, &sink->saved_ui_alpha);
        }
    }

    if (sink->wake != NULL) {
        (void)rt_sem_delete(sink->wake);
        sink->wake = NULL;
    }
    if (sink->lock != NULL) {
        (void)rt_mutex_delete(sink->lock);
        sink->lock = NULL;
    }
    sink->thread = NULL;
}

bool carplay_video_sink_is_running(void)
{
    return s_sink.running;
}

size_t carplay_video_sink_pending(void)
{
    video_sink_t *sink = &s_sink;
    if (!sink->running || sink->lock == NULL) return 0U;
    if (rt_mutex_take(sink->lock, RT_WAITING_FOREVER) != RT_EOK) return 0U;
    size_t pending =
        (sink->head + SINK_QUEUE_LENGTH - sink->tail) % SINK_QUEUE_LENGTH;
    (void)rt_mutex_release(sink->lock);
    return pending;
}

void carplay_video_sink_stats(unsigned *nals, unsigned *frames,
                              unsigned *dropped, unsigned *errors)
{
    if (nals != NULL) *nals = s_sink.nal_count;
    if (frames != NULL) *frames = s_sink.frame_count;
    if (dropped != NULL) *dropped = s_sink.dropped_nals;
    if (errors != NULL) *errors = s_sink.decode_errors;
}

/* ------------------------------------------------------------------ */
/* Bring-up shell command: decode an Annex-B file without a controller */
/* ------------------------------------------------------------------ */

#define VIDEO_TEST_READ_BYTES (64U * 1024U)
#define VIDEO_TEST_CARRY_BYTES (SINK_MAX_NAL_BYTES + VIDEO_TEST_READ_BYTES)

static size_t find_last_start_code(const uint8_t *data, size_t len)
{
    if (len < 3U) return SIZE_MAX;
    size_t i = len - 3U;
    for (;;) {
        if (data[i] == 0x00 && data[i + 1U] == 0x00 && data[i + 2U] == 0x01) {
            return i;
        }
        if (i == 0U) return SIZE_MAX;
        --i;
    }
}

static void wait_for_decode_room(void)
{
    unsigned guard = 2000U;
    while (carplay_video_sink_is_running() &&
           carplay_video_sink_pending() >= 8U && guard-- != 0U) {
        rt_thread_mdelay(5);
    }
}

static int play_h264_file(const char *path)
{
    bool started_here = false;
    if (!carplay_video_sink_is_running()) {
        int result = carplay_video_sink_start(1280U, 480U);
        if (result != RTCP_OK) {
            rt_kprintf("[" TAG "] cannot start the sink: %d\n", result);
            return result;
        }
        started_here = true;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        rt_kprintf("[" TAG "] cannot open %s\n", path);
        if (started_here) carplay_video_sink_stop();
        return RTCP_ENOENT;
    }
    uint8_t *carry = malloc(VIDEO_TEST_CARRY_BYTES);
    if (carry == NULL) {
        fclose(file);
        if (started_here) carplay_video_sink_stop();
        return RTCP_ENOMEM;
    }

    size_t carry_len = 0U;
    size_t fed_bytes = 0U;
    unsigned fed_batches = 0U;
    bool eof = false;
    while (carplay_video_sink_is_running()) {
        if (!eof && carry_len < VIDEO_TEST_CARRY_BYTES) {
            size_t got = fread(carry + carry_len, 1U,
                               VIDEO_TEST_CARRY_BYTES - carry_len, file);
            if (got == 0U) eof = true;
            carry_len += got;
        }

        const uint8_t *start = find_start_code(carry, carry + carry_len);
        if (start == NULL) {
            if (eof) break;
            /* Keep a possible split start code, drop the rest. */
            size_t keep = carry_len > 2U ? 2U : carry_len;
            memmove(carry, carry + carry_len - keep, keep);
            carry_len = keep;
            continue;
        }
        if (start != carry) {
            size_t drop = (size_t)(start - carry);
            memmove(carry, start, carry_len - drop);
            carry_len -= drop;
        }

        size_t next = find_last_start_code(carry + 3U, carry_len - 3U);
        if (next == SIZE_MAX) {
            if (!eof && carry_len < VIDEO_TEST_CARRY_BYTES) continue;
            /* One NAL fills the buffer (or ends the file): submit it whole. */
            next = carry_len;
        } else {
            next += 3U;
        }
        if (next == 0U) continue;

        carplay_video_sink_frame(false, carry, next, 0U, NULL);
        fed_bytes += next;
        ++fed_batches;
        memmove(carry, carry + next, carry_len - next);
        carry_len -= next;
        if (eof && carry_len < 3U) break;
        wait_for_decode_room();
    }

    free(carry);
    fclose(file);
    unsigned nals = 0U, frames = 0U, dropped = 0U, errors = 0U;
    carplay_video_sink_stats(&nals, &frames, &dropped, &errors);
    rt_kprintf("[" TAG "] played %s: %u batches, %u bytes, decoder %u NALs/%u frames, dropped %u, errors %u\n",
               path, fed_batches, (unsigned)fed_bytes, nals, frames, dropped,
               errors);
    return RTCP_OK;
}

static int cmd_carplay_video(int argc, char **argv)
{
    if (argc < 2) {
        rt_kprintf("usage: carplay_video start [width height] | stop | status | play <annexb-file>\n");
        return 0;
    }
    if (strcmp(argv[1], "start") == 0) {
        unsigned width = argc >= 4 ? (unsigned)atoi(argv[2]) : 1280U;
        unsigned height = argc >= 4 ? (unsigned)atoi(argv[3]) : 480U;
        int result = carplay_video_sink_start((uint16_t)width, (uint16_t)height);
        rt_kprintf("[" TAG "] start %ux%u: %d\n", width, height, result);
        return 0;
    }
    if (strcmp(argv[1], "stop") == 0) {
        carplay_video_sink_stop();
        rt_kprintf("[" TAG "] stopped\n");
        return 0;
    }
    if (strcmp(argv[1], "status") == 0) {
        unsigned nals = 0U, frames = 0U, dropped = 0U, errors = 0U;
        carplay_video_sink_stats(&nals, &frames, &dropped, &errors);
        rt_kprintf("[" TAG "] %s, pending %u, NALs %u, frames %u, dropped %u, errors %u\n",
                   carplay_video_sink_is_running() ? "running" : "stopped",
                   (unsigned)carplay_video_sink_pending(), nals, frames,
                   dropped, errors);
        return 0;
    }
    if (strcmp(argv[1], "play") == 0) {
        if (argc < 3) {
            rt_kprintf("usage: carplay_video play <annexb-file>\n");
            return 0;
        }
        (void)play_h264_file(argv[2]);
        return 0;
    }
    rt_kprintf("unknown carplay_video subcommand: %s\n", argv[1]);
    return 0;
}

MSH_CMD_EXPORT_ALIAS(cmd_carplay_video, carplay_video,
                     Test the MPP video path: start/stop/status/play file);
