/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual sink + SDK public headers; model a two-slot MPP decoder. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#ifndef VIDEO_SINK_SOURCE
#define VIDEO_SINK_SOURCE "../../examples/rtthread_app/video_sink_lubanlite.c"
#endif
#include VIDEO_SINK_SOURCE

static unsigned slots, decoded, destroyed, delays;
static unsigned last_config_size;
static bool pressure;
static bool reference_mode, reference_active, fail_next;
static unsigned packet_type;
static unsigned render_remaining, render_id, returned[16], return_count;
static unsigned updates, vsyncs, displayed_id;
static uint8_t packet_data[1024];
static const uint8_t config[] = {0,0,0,1,0x67,0x42,0x00,0x1f,0x80,
                                0,0,0,1,0x68,0xce,0x38,0x80};
static const uint8_t frame[] = {0,0,0,1,0x65,0xb8,0x80};

rt_base_t rt_hw_interrupt_disable(void) { return 0; }
void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
uint32_t rt_tick_get_millisecond(void) { return 100; }
int rt_kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int rt_mutex_take(rt_mutex_t m, int t) { (void)m; (void)t; return 0; }
int rt_mutex_release(rt_mutex_t m) { (void)m; return 0; }
int rt_sem_release(rt_sem_t s) { (void)s; return 0; }
int rt_sem_take(rt_sem_t s, int t) {
    (void)s; (void)t;
    if (s_sink.head == s_sink.tail) { s_sink.stop = true; return -1; }
    return 0;
}
void rt_thread_mdelay(int ms) { (void)ms; assert(++delays < 10); }
int rt_device_control(rt_device_t d, int cmd, void *arg) {
    (void)d;
    if (cmd == AICFB_UPDATE_LAYER_CONFIG) {
        struct aicfb_layer_data *layer = arg;
        if (layer->enable) { ++updates; displayed_id = layer->buf.phy_addr[0]; }
    }
    if (cmd == AICFB_WAIT_FOR_VSYNC) ++vsyncs;
    return 0;
}
struct mpp_decoder *mpp_decoder_create(enum mpp_codec_type t) {
    (void)t; return (struct mpp_decoder *)(uintptr_t)1;
}
void mpp_decoder_destory(struct mpp_decoder *d) {
    (void)d; ++destroyed; slots = 0; reference_active = false;
}
int mpp_decoder_init(struct mpp_decoder *d, struct decode_config *c) {
    (void)d; assert(c->packet_count == 2); return 0;
}
int mpp_decoder_get_packet(struct mpp_decoder *d, struct mpp_packet *p, int size) {
    (void)d;
    if (slots == 2) return DEC_NO_EMPTY_PACKET;
    assert(size > 0 && (size_t)size <= sizeof(packet_data));
    p->data = packet_data; return DEC_OK;
}
int mpp_decoder_put_packet(struct mpp_decoder *d, struct mpp_packet *p) {
    (void)d;
    assert(slots < 2); ++slots;
    const uint8_t *start = find_start_code(p->data, (uint8_t *)p->data + p->size);
    packet_type = start != NULL ? start[3] & 31U : 0U;
    if (p->size == (int)sizeof(config)) {
        last_config_size = (unsigned)p->size;
    }
    return DEC_OK;
}
int mpp_decoder_decode(struct mpp_decoder *d) {
    (void)d;
    if (!slots) return DEC_NO_READY_PACKET;
    --slots; ++decoded;
    if (fail_next) { fail_next = false; reference_active = false; return -1; }
    if (reference_mode) {
        if (packet_type == 5) reference_active = true;
        if (packet_type == 1) assert(reference_active);
    }
    /* First retry exercises rendering backpressure in pump_decoder(). */
    if (pressure) { pressure = false; return DEC_NO_EMPTY_FRAME; }
    return DEC_OK;
}
int mpp_decoder_get_frame(struct mpp_decoder *d, struct mpp_frame *f) {
    (void)d;
    if (!render_remaining) return DEC_NO_RENDER_FRAME;
    --render_remaining;
    memset(f, 0, sizeof(*f));
    f->id = (int)render_id++;
    f->buf.phy_addr[0] = (unsigned)f->id;
    f->buf.size.width = 1024;
    f->buf.size.height = 600;
    return DEC_OK;
}
int mpp_decoder_put_frame(struct mpp_decoder *d, struct mpp_frame *f) {
    (void)d;
    assert(return_count < 16);
    if (f->id == 99) assert(vsyncs == 1); /* old scanout held until swap */
    returned[return_count++] = (unsigned)f->id;
    return DEC_OK;
}

/* Lifecycle is not invoked by the worker regression. */
rt_device_t rt_device_find(const char *name) { (void)name; abort(); }
rt_sem_t rt_sem_create(const char *n, unsigned v, int f) {
    (void)n; (void)v; (void)f; abort();
}
rt_mutex_t rt_mutex_create(const char *n, int f) { (void)n; (void)f; abort(); }
int rt_sem_delete(rt_sem_t s) { (void)s; abort(); }
int rt_mutex_delete(rt_mutex_t m) { (void)m; abort(); }
rt_thread_t rt_thread_create(const char *n, void (*fn)(void *), void *arg,
                             unsigned stack, unsigned prio, unsigned tick) {
    (void)n; (void)fn; (void)arg; (void)stack; (void)prio; (void)tick; abort();
}
int rt_thread_startup(rt_thread_t t) { (void)t; abort(); }

static void reset(void) {
    memset(&s_sink, 0, sizeof(s_sink));
    s_sink.running = true;
    s_sink.lock = (rt_mutex_t)(uintptr_t)1;
    s_sink.decoder = (struct mpp_decoder *)(uintptr_t)1;
    slots = decoded = destroyed = delays = last_config_size = 0;
    pressure = false;
    reference_mode = reference_active = fail_next = false;
    render_remaining = return_count = updates = vsyncs = displayed_id = 0;
    render_id = 1;
}
int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "queue") == 0) {
        reset();
        for (unsigned i = 0; i < 10; ++i)
            carplay_video_sink_frame(false, frame, sizeof(frame), 0, NULL);
        sink_thread(&s_sink);
        assert(decoded == 10 && s_sink.nal_count == 10 && delays == 0);
        puts("bounded packet queue progresses without waiting");
        return 0;
    }
    reset();
    s_sink.frame_count = 8; /* configuration after an established stream */
    carplay_video_sink_frame(true, config, sizeof(config), 0, NULL);
    assert(carplay_video_sink_pending() == 1);
    for (unsigned i = 0; i < 10; ++i)
        carplay_video_sink_frame(false, frame, sizeof(frame), 0, NULL);
    sink_thread(&s_sink);
    assert(destroyed == 1 && s_sink.decoder_resets == 1);
    assert(last_config_size == sizeof(config));
    assert(decoded == 11 && s_sink.nal_count == 11 && slots == 0);
    assert(s_sink.decode_errors == 0 && delays == 0);

    reset(); /* Repeated SPS/PPS must preserve an established reference chain. */
    reference_mode = true;
    render_remaining = 1; /* establish frame_count before repeated config */
    uint8_t inter[] = {0,0,0,1,0x41,0xb8,0x80};
    carplay_video_sink_frame(true, config, sizeof(config), 0, NULL);
    carplay_video_sink_frame(false, frame, sizeof(frame), 0, NULL);
    carplay_video_sink_frame(true, config, sizeof(config), 0, NULL);
    carplay_video_sink_frame(false, inter, sizeof(inter), 0, NULL);
    sink_thread(&s_sink);
    assert(destroyed == 0 && decoded == 3 && reference_active);
    assert(s_sink.repeated_configs == 1 && s_sink.decode_errors == 0);

    reset(); /* Changed SPS/PPS: skip dependent picture, replay config at IDR. */
    reference_mode = true;
    uint8_t changed[sizeof(config)];
    memcpy(changed, config, sizeof(config)); changed[7] ^= 1;
    carplay_video_sink_frame(true, config, sizeof(config), 0, NULL);
    carplay_video_sink_frame(false, frame, sizeof(frame), 0, NULL);
    carplay_video_sink_frame(true, changed, sizeof(changed), 0, NULL);
    carplay_video_sink_frame(false, inter, sizeof(inter), 0, NULL);
    carplay_video_sink_frame(false, frame, sizeof(frame), 0, NULL);
    carplay_video_sink_frame(false, inter, sizeof(inter), 0, NULL);
    sink_thread(&s_sink);
    assert(destroyed == 1 && decoded == 5 && s_sink.waiting_nals == 1);
    assert(reference_active && s_sink.decode_errors == 0);

    reset(); /* Poisoned decoder is quarantined until cached config + IDR. */
    reference_mode = true;
    nal_entry_t cfg = {.data = (uint8_t *)config, .len = sizeof(config), .configure = true};
    assert(accept_configuration(&s_sink, &cfg));
    assert(submit_nal(&s_sink, &cfg) == 0); pump_decoder(&s_sink);
    nal_entry_t idr = {.data = (uint8_t *)frame, .len = sizeof(frame)};
    assert(prepare_picture(&s_sink, &idr));
    assert(submit_nal(&s_sink, &idr) == 0); fail_next = true; pump_decoder(&s_sink);
    assert(s_sink.wait_idr && s_sink.recovery_pending);
    carplay_video_sink_frame(false, inter, sizeof(inter), 0, NULL);
    carplay_video_sink_frame(false, frame, sizeof(frame), 0, NULL);
    carplay_video_sink_frame(false, inter, sizeof(inter), 0, NULL);
    sink_thread(&s_sink);
    assert(destroyed == 1 && decoded == 5 && s_sink.waiting_nals == 1);
    assert(reference_active && s_sink.decode_errors == 1);

    reset(); /* Input loss invalidates only entries following the gap. */
    reference_mode = true;
    uint8_t later_idr_slice[] = {0,0,0,1,0x65,0x40,0x80};
    carplay_video_sink_frame(true, config, sizeof(config), 0, NULL);
    carplay_video_sink_frame(false, frame, sizeof(frame), 0, NULL);
    carplay_video_sink_frame(false, inter, sizeof(inter), 0, NULL);
    drop_nal(&s_sink); /* next queued packet belongs after this missing input */
    carplay_video_sink_frame(false, inter, sizeof(inter), 0, NULL);
    carplay_video_sink_frame(false, later_idr_slice, sizeof(later_idr_slice), 0, NULL);
    carplay_video_sink_frame(false, frame, sizeof(frame), 0, NULL);
    carplay_video_sink_frame(false, inter, sizeof(inter), 0, NULL);
    sink_thread(&s_sink);
    assert(destroyed == 1 && decoded == 6 && s_sink.waiting_nals == 2);
    assert(reference_active && s_sink.decode_errors == 0);

    reset(); /* retry must decode to free slots on this same thread */
    slots = 2; pressure = true;
    nal_entry_t nal = {.data = (uint8_t *)frame, .len = sizeof(frame)};
    assert(submit_nal(&s_sink, &nal) == 0);
    pump_decoder(&s_sink);
    assert(decoded == 3 && slots == 0 && delays == 1);

    reset(); /* full queue must not admit only half a configuration */
    for (unsigned i = 0; i < SINK_QUEUE_LENGTH - 1; ++i)
        carplay_video_sink_frame(false, frame, sizeof(frame), 0, NULL);
    carplay_video_sink_frame(true, config, sizeof(config), 0, NULL);
    assert(s_sink.dropped_nals == 1);
    sink_thread(&s_sink);
    assert(destroyed == 0 && decoded == SINK_QUEUE_LENGTH - 1);
    reset(); /* Coalesce decoded backlog, preserve hardware scanout lifetime. */
    s_sink.have_frame = true;
    s_sink.held.id = 99;
    render_remaining = 3;
    assert(!s_sink.ui_hidden);
    assert(drain_frames(&s_sink) == 3);
    assert(updates == 1 && vsyncs == 1 && displayed_id == 3);
    assert(s_sink.ui_hidden); /* launcher yields only when output is ready */
    assert(return_count == 3 && returned[0] == 1 && returned[1] == 2 && returned[2] == 99);
    assert(s_sink.frame_count == 1 && s_sink.skipped_frames == 2 && s_sink.held.id == 3);
    puts("Luban-Lite actual video sink regression passed");
    return 0;
}
