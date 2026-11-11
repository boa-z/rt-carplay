/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Luban-Lite touch panel: RT-Thread CTP device -> CarPlay touch events.
 *
 * The RT-Thread touch core raises the panel interrupt; following the SDK
 * test-ctp example, the rx indication disables the IRQ and a dedicated
 * thread reads the point buffer, forwards the events and re-enables the
 * IRQ.  The driver already applies flip/rotate/scale/crop from the board
 * configuration, so this file only scales the reported range to the display
 * geometry.  The reader thread calls the callback; the application turns
 * that into carplay_app_touch() queue entries and never does HID work here.
 */
#include "touch_panel_lubanlite.h"

#include <rtthread.h>
#include <rtdevice.h>

#include <string.h>

#include "rtcp/input.h"
#include "rtcp/rtcp.h"

#define TAG "carplay-touch"
#define PANEL_THREAD_STACK_BYTES (4U * 1024U)
#define PANEL_THREAD_PRIORITY 22U
#define PANEL_THREAD_TICK 10U
#define PANEL_WAKE_TIMEOUT_MS 200U

/* The SDK names the device after the selected CTP driver. */
#if defined(AIC_TOUCH_PANEL_NAME)
#define PANEL_DEVICE_NAME AIC_TOUCH_PANEL_NAME
#else
#define PANEL_DEVICE_NAME "gt911"
#endif

typedef struct {
    rt_device_t dev;
    rt_thread_t thread;
    rt_sem_t wake;
    struct rt_touch_data *points;
    uint16_t point_count;
    uint16_t range_x;
    uint16_t range_y;
    uint16_t width;
    uint16_t height;
    unsigned reads;
    unsigned events;
    carplay_touch_panel_cb_t callback;
    void *context;
    volatile bool stop;
    volatile bool running;
    volatile bool thread_running;
} touch_panel_t;

static touch_panel_t s_panel;

static uint16_t scale_axis(uint16_t value, uint16_t range, uint16_t extent)
{
    if (range == 0U) return 0U;
    uint32_t scaled = ((uint32_t)value * (uint32_t)extent) / (uint32_t)range;
    if (scaled > (uint32_t)extent) scaled = (uint32_t)extent;
    return (uint16_t)scaled;
}

static rt_err_t panel_rx(rt_device_t dev, rt_size_t size)
{
    (void)size;
    touch_panel_t *panel = &s_panel;
    if (panel->dev != dev) return RT_EOK;
    /* The example disables the IRQ until the thread has drained the panel;
     * otherwise the level-triggered line keeps the ISR busy. */
    (void)rt_device_control(dev, RT_TOUCH_CTRL_DISABLE_INT, RT_NULL);
    if (panel->wake != NULL) (void)rt_sem_release(panel->wake);
    return RT_EOK;
}

static void panel_thread(void *argument)
{
    touch_panel_t *panel = argument;
    panel->thread_running = true;

    while (!panel->stop) {
        if (rt_sem_take(panel->wake,
                        rt_tick_from_millisecond(PANEL_WAKE_TIMEOUT_MS)) !=
            RT_EOK) {
            continue;
        }
        if (panel->stop) break;

        ++panel->reads;
        rt_size_t count = rt_device_read(panel->dev, 0, panel->points,
                                         panel->point_count);
        for (rt_size_t i = 0U; i < count && i < panel->point_count; ++i) {
            const struct rt_touch_data *point = &panel->points[i];
            uint8_t phase;
            switch (point->event) {
            case RT_TOUCH_EVENT_DOWN: phase = RTCP_TOUCH_DOWN; break;
            case RT_TOUCH_EVENT_MOVE: phase = RTCP_TOUCH_MOVE; break;
            case RT_TOUCH_EVENT_UP: phase = RTCP_TOUCH_UP; break;
            default: continue;
            }
            uint16_t x = scale_axis(point->x_coordinate, panel->range_x,
                                    panel->width);
            uint16_t y = scale_axis(point->y_coordinate, panel->range_y,
                                    panel->height);
            ++panel->events;
            panel->callback(phase, point->track_id, x, y, panel->context);
        }
        (void)rt_device_control(panel->dev, RT_TOUCH_CTRL_ENABLE_INT, RT_NULL);
    }

    panel->thread_running = false;
}

int carplay_touch_panel_start(uint16_t width, uint16_t height,
                              carplay_touch_panel_cb_t callback,
                              void *context)
{
    touch_panel_t *panel = &s_panel;
    if (panel->running || panel->thread_running) return RTCP_ESTATE;
    if (width == 0U || height == 0U || callback == NULL) return RTCP_EINVAL;

    memset(panel, 0, sizeof(*panel));
    panel->width = width;
    panel->height = height;
    panel->callback = callback;
    panel->context = context;
    panel->range_x = width;
    panel->range_y = height;

    panel->dev = rt_device_find(PANEL_DEVICE_NAME);
    if (panel->dev == NULL) {
        rt_kprintf("[" TAG "] touch device %s not found\n", PANEL_DEVICE_NAME);
        return RTCP_ENOENT;
    }
    rt_err_t open_result = rt_device_open(panel->dev, RT_DEVICE_FLAG_INT_RX);
    if (open_result != RT_EOK) {
        rt_kprintf("[" TAG "] cannot open %s rc=%d (check device ownership)\n",
                   PANEL_DEVICE_NAME, (int)open_result);
        panel->dev = NULL;
        return RTCP_EIO;
    }

    /* Opening initializes the IRQ. Keep it disabled until the buffer,
     * callback and reader are ready, then explicitly enable it. */
    (void)rt_device_control(panel->dev, RT_TOUCH_CTRL_DISABLE_INT, RT_NULL);
    struct rt_touch_info info;
    memset(&info, 0, sizeof(info));
    if (rt_device_control(panel->dev, RT_TOUCH_CTRL_GET_INFO, &info) != RT_EOK ||
        info.point_num == 0U) {
        rt_kprintf("[" TAG "] %s did not report its geometry\n",
                   PANEL_DEVICE_NAME);
        (void)rt_device_close(panel->dev);
        panel->dev = NULL;
        return RTCP_EIO;
    }
    panel->point_count = info.point_num;
    if (info.range_x > 0) panel->range_x = (uint16_t)info.range_x;
    if (info.range_y > 0) panel->range_y = (uint16_t)info.range_y;

    panel->points =
        rt_malloc(sizeof(struct rt_touch_data) * (size_t)panel->point_count);
    panel->wake = rt_sem_create("catp", 0, RT_IPC_FLAG_FIFO);
    if (panel->points == NULL || panel->wake == NULL) {
        if (panel->points != NULL) rt_free(panel->points);
        if (panel->wake != NULL) (void)rt_sem_delete(panel->wake);
        (void)rt_device_close(panel->dev);
        panel->dev = NULL;
        panel->points = NULL;
        panel->wake = NULL;
        return RTCP_ENOMEM;
    }

    (void)rt_device_set_rx_indicate(panel->dev, panel_rx);
    panel->stop = false;
    panel->running = true;
    panel->thread = rt_thread_create("carplay-tp", panel_thread, panel,
                                     PANEL_THREAD_STACK_BYTES,
                                     PANEL_THREAD_PRIORITY, PANEL_THREAD_TICK);
    if (panel->thread == NULL) {
        panel->running = false;
        (void)rt_device_set_rx_indicate(panel->dev, NULL);
        (void)rt_device_close(panel->dev);
        rt_free(panel->points);
        (void)rt_sem_delete(panel->wake);
        memset(panel, 0, sizeof(*panel));
        return RTCP_ENOMEM;
    }
    /* Set before scheduling so an immediate stop cannot free resources
     * before the newly created reader has entered its function. */
    panel->thread_running = true;
    if (rt_thread_startup(panel->thread) != RT_EOK) {
        panel->thread_running = false;
        panel->running = false;
        (void)rt_thread_delete(panel->thread);
        (void)rt_device_set_rx_indicate(panel->dev, NULL);
        (void)rt_device_close(panel->dev);
        rt_free(panel->points);
        (void)rt_sem_delete(panel->wake);
        memset(panel, 0, sizeof(*panel));
        return RTCP_EIO;
    }
    (void)rt_device_control(panel->dev, RT_TOUCH_CTRL_ENABLE_INT, RT_NULL);

    rt_kprintf("[" TAG "] %s: %u point(s), %ux%u -> %ux%u\n",
               PANEL_DEVICE_NAME, (unsigned)panel->point_count,
               (unsigned)panel->range_x, (unsigned)panel->range_y,
               (unsigned)width, (unsigned)height);
    return RTCP_OK;
}

void carplay_touch_panel_stop(void)
{
    touch_panel_t *panel = &s_panel;
    if (!panel->running) return;

    panel->running = false;
    panel->stop = true;
    if (panel->wake != NULL) (void)rt_sem_release(panel->wake);
    /* The reader can be inside a short I2C read; give it a bound, then leak
     * rather than close a device still in use. */
    unsigned guard = 100U;
    while (panel->thread_running && guard-- != 0U) {
        rt_thread_mdelay(10);
    }
    if (panel->thread_running) {
        rt_kprintf("[" TAG "] reader did not stop; resources left allocated\n");
        return;
    }

    if (panel->dev != NULL) {
        (void)rt_device_control(panel->dev, RT_TOUCH_CTRL_DISABLE_INT,
                                RT_NULL);
        (void)rt_device_set_rx_indicate(panel->dev, NULL);
        (void)rt_device_close(panel->dev);
        panel->dev = NULL;
    }
    if (panel->points != NULL) {
        rt_free(panel->points);
        panel->points = NULL;
    }
    if (panel->wake != NULL) {
        (void)rt_sem_delete(panel->wake);
        panel->wake = NULL;
    }
    panel->thread = NULL;
    panel->callback = NULL;
    panel->context = NULL;
    panel->point_count = 0U;
    panel->thread_running = false;
}

bool carplay_touch_panel_is_running(void)
{
    return s_panel.running;
}

uint16_t carplay_touch_panel_points(void)
{
    return s_panel.running ? s_panel.point_count : 0U;
}

void carplay_touch_panel_stats(unsigned *reads, unsigned *events)
{
    if (reads != NULL) *reads = s_panel.reads;
    if (events != NULL) *events = s_panel.events;
}
