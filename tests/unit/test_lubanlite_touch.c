/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../examples/rtthread_app/touch_panel_lubanlite.c"

static unsigned tokens, callbacks, closes, allocations;
static bool opened, irq, startup_fail, busy;
static void (*entry)(void *);
static void *entry_arg;
static rt_err_t (*rx)(rt_device_t, rt_size_t);
static int dev;
static struct { uint8_t phase; uint16_t id, x, y; } delivered[3];

int rt_kprintf(const char *s, ...) { (void)s; return 0; }
int rt_tick_from_millisecond(unsigned ms) { return (int)ms; }
void *rt_malloc(size_t n) { ++allocations; return malloc(n); }
void rt_free(void *p) { assert(allocations > 0); --allocations; free(p); }
rt_device_t rt_device_find(const char *name) {
    assert(strcmp(name, "gt911") == 0); return &dev;
}
int rt_device_open(rt_device_t d, unsigned flag) {
    assert(d == &dev && flag == RT_DEVICE_FLAG_INT_RX);
    if (busy) return -16;
    assert(!opened); opened = true; irq = true; return 0;
}
int rt_device_close(rt_device_t d) {
    assert(d == &dev && opened);
    assert(!s_panel.thread_running);
    opened = false; irq = false; ++closes; return 0;
}
int rt_device_control(rt_device_t d, int cmd, void *arg) {
    assert(d == &dev && opened);
    switch (cmd) {
    case RT_TOUCH_CTRL_DISABLE_INT: irq = false; break;
    case RT_TOUCH_CTRL_ENABLE_INT:
        assert(rx != NULL && s_panel.points != NULL && s_panel.thread != NULL);
        irq = true; break;
    case RT_TOUCH_CTRL_GET_INFO:
        *(struct rt_touch_info *)arg = (struct rt_touch_info){3, 2048, 1200};
        break;
    default: abort();
    }
    return 0;
}
int rt_device_set_rx_indicate(rt_device_t d,
                              rt_err_t (*fn)(rt_device_t, rt_size_t)) {
    assert(d == &dev); rx = fn; return 0;
}
rt_size_t rt_device_read(rt_device_t d, int pos, void *buf, rt_size_t count) {
    assert(d == &dev && pos == 0 && count == 3 && !irq);
    struct rt_touch_data data[] = {
        {RT_TOUCH_EVENT_DOWN, 7, 2048, 1200},
        {RT_TOUCH_EVENT_MOVE, 7, 1024, 600},
        {RT_TOUCH_EVENT_UP,   7, 1024, 600},
    };
    memcpy(buf, data, sizeof(data)); return 3;
}
rt_sem_t rt_sem_create(const char *n, unsigned value, int f) {
    (void)n; (void)f; tokens = value; return &tokens;
}
int rt_sem_release(rt_sem_t s) { assert(s == &tokens); ++tokens; return 0; }
int rt_sem_delete(rt_sem_t s) { assert(s == &tokens); return 0; }
int rt_sem_take(rt_sem_t s, int timeout) {
    assert(s == &tokens && timeout == 200);
    if (tokens) { --tokens; return 0; }
    s_panel.stop = true; return 0;
}
rt_thread_t rt_thread_create(const char *n, void (*fn)(void *), void *arg,
                             unsigned stack, unsigned prio, unsigned tick) {
    (void)n; (void)stack; (void)prio; (void)tick;
    entry = fn; entry_arg = arg; return &entry;
}
int rt_thread_startup(rt_thread_t t) {
    assert(t == &entry && s_panel.thread_running && !irq);
    return startup_fail ? -1 : 0;
}
int rt_thread_delete(rt_thread_t t) { assert(t == &entry); entry = NULL; return 0; }
void rt_thread_mdelay(int ms) {
    assert(ms == 10 && entry != NULL && allocations == 1);
    /* Schedule the pending reader during an immediate stop; its buffer
     * and callback must still exist until it exits. */
    entry(entry_arg); entry = NULL;
}
static void callback(uint8_t phase, uint16_t id, uint16_t x, uint16_t y, void *ctx) {
    assert(ctx == &dev && callbacks < 3);
    delivered[callbacks].phase = phase;
    delivered[callbacks].id = id;
    delivered[callbacks].x = x;
    delivered[callbacks++].y = y;
}
int main(void) {
    busy = true;
    assert(carplay_touch_panel_start(1024, 600, callback, &dev) == RTCP_EIO);
    assert(!opened && closes == 0 && allocations == 0);
    busy = false;
    assert(carplay_touch_panel_start(1024, 600, callback, &dev) == 0);
    assert(irq && rx != NULL && carplay_touch_panel_points() == 3);
    assert(rx(&dev, 1) == 0 && !irq);
    entry(entry_arg); entry = NULL;
    assert(callbacks == 3 && irq);
    assert(delivered[0].phase == RTCP_TOUCH_DOWN && delivered[0].id == 7);
    assert(delivered[0].x == 1024 && delivered[0].y == 600);
    assert(delivered[1].phase == RTCP_TOUCH_MOVE && delivered[1].x == 512 && delivered[1].y == 300);
    assert(delivered[2].phase == RTCP_TOUCH_UP);
    unsigned reads, events;
    carplay_touch_panel_stats(&reads, &events);
    assert(reads == 1 && events == 3);
    carplay_touch_panel_stop();
    assert(!opened && !irq && rx == NULL && allocations == 0);

    assert(carplay_touch_panel_start(1024, 600, callback, &dev) == 0);
    carplay_touch_panel_stop(); /* reader has not entered yet */
    assert(!opened && allocations == 0 && entry == NULL);

    startup_fail = true;
    assert(carplay_touch_panel_start(1024, 600, callback, &dev) == RTCP_EIO);
    assert(!opened && !irq && rx == NULL && allocations == 0 && entry == NULL);
    puts("actual touch reader: ownership, IRQ, coordinates, events and teardown passed");
    return 0;
}
