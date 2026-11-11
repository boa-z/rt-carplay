/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Informational LVGL launch page + one owner for the automatic wired demo.
 * LVGL objects are touched only by the LVGL thread. USB, TLS and shutdown
 * run on a separate worker. The receiver retains exclusive panel ownership. */
#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "carplay_app.h"
#include "carplay_usb.h"
#include "video_sink_lubanlite.h"
#include "rtcp/rtcp.h"

typedef enum {
    DEMO_BOOT, DEMO_WAIT, DEMO_PAIR, DEMO_NETWORK, DEMO_CONNECT,
    DEMO_LIVE, DEMO_CLOSING, DEMO_FAILED
} demo_stage_t;

typedef struct {
    demo_stage_t stage;
    int error;
    char pin[16];
} demo_status_t;

static rt_mutex_t status_lock;
static demo_status_t status;
static lv_obj_t *heading, *description, *pin_label, *badge;
static lv_obj_t *steps[3];

static void publish(demo_stage_t stage, int error)
{
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    status.stage = stage;
    status.error = error;
    (void)rt_mutex_release(status_lock);
    rt_kprintf("[carplay-demo] stage=%u error=%d\n", (unsigned)stage, error);
}

/* Leave the hook armed for reconnect. Never stop the receiver while an
 * iAP2 worker still borrows its strings or its TLS carkit stream. */
static void close_session(void)
{
    publish(DEMO_CLOSING, 0);
    while (carplay_usb_close_session() != RTCP_OK)
        rt_thread_mdelay(250);
    (void)carplay_app_stop();
}

static void demo_worker(void *argument)
{
    (void)argument;
    int ret = carplay_usb_open();
    if (ret != RTCP_OK) { publish(DEMO_FAILED, ret); return; }

    for (;;) {
        ret = carplay_app_autostart_wired();
        if (ret != RTCP_OK) { publish(DEMO_FAILED, ret); return; }
        (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
        snprintf(status.pin, sizeof(status.pin), "%s", carplay_app_pairing_code());
        (void)rt_mutex_release(status_lock);
        publish(DEMO_WAIT, 0);
        while (!carplay_usb_attached()) rt_thread_mdelay(200);

        publish(DEMO_PAIR, 0);
        ret = carplay_usb_pair();
        if (ret == RTCP_OK) {
            publish(DEMO_NETWORK, 0);
            ret = RTCP_ETIMEDOUT;
            for (unsigned wait = 0; wait < 15000U && carplay_usb_attached(); wait += 100U) {
                carplay_app_wired_info_t endpoint;
                if (carplay_app_wired_info(&endpoint) == RTCP_OK) { ret = RTCP_OK; break; }
                rt_thread_mdelay(100);
            }
        }
        if (ret == RTCP_OK) {
            publish(DEMO_CONNECT, 0);
            ret = carplay_usb_start_iap2();
        }
        if (ret == RTCP_OK) {
            ret = RTCP_ETIMEDOUT;
            for (unsigned wait = 0; wait < 90000U && carplay_usb_attached() &&
                                      carplay_usb_iap2_running(); wait += 200U) {
                unsigned frames = 0;
                carplay_video_sink_stats(NULL, &frames, NULL, NULL);
                if (frames != 0U) { ret = RTCP_OK; break; }
                rt_thread_mdelay(200);
            }
        }
        if (ret == RTCP_OK) {
            publish(DEMO_LIVE, 0);
            while (carplay_usb_attached() && carplay_usb_iap2_running() &&
                   carplay_app_is_running()) rt_thread_mdelay(250);
        }
        close_session();
        if (carplay_usb_attached()) {
            /* One attempt per attachment: never spin pairing/key generation
             * against a rejected trust dialog or an incompatible phone. */
            publish(DEMO_FAILED, ret != RTCP_OK ? ret : RTCP_EIO);
            while (carplay_usb_attached()) rt_thread_mdelay(200);
        }
    }
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y,
                        int width, uint32_t color, bool large)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
#if LV_FONT_MONTSERRAT_24
    if (large) lv_obj_set_style_text_font(obj, &lv_font_montserrat_24, 0);
#else
    (void)large;
#endif
    return obj;
}

static void update_page(lv_timer_t *timer)
{
    (void)timer;
    static int last_stage = -1;
    demo_status_t snapshot;
    if (rt_mutex_take(status_lock, RT_WAITING_NO) != RT_EOK) return;
    snapshot = status;
    (void)rt_mutex_release(status_lock);
    if ((int)snapshot.stage == last_stage) return;
    last_stage = (int)snapshot.stage;
    const char *title, *detail, *tag;
    switch (snapshot.stage) {
    case DEMO_WAIT:
        title = "Connect your iPhone";
        detail = "Plug an unlocked iPhone into the USB port.\nCarPlay will connect automatically.";
        tag = "READY"; break;
    case DEMO_PAIR:
        title = "Trust this connection";
        detail = "Keep your iPhone unlocked.\nTap Trust on the phone if asked. This may take a moment.";
        tag = "PAIRING"; break;
    case DEMO_NETWORK:
        title = "Preparing the connection";
        detail = "Your iPhone is paired.\nSetting up the wired connection...";
        tag = "CONNECTING"; break;
    case DEMO_CONNECT:
        title = "Starting CarPlay";
        detail = "Allow CarPlay on your iPhone if asked.\nYour apps will appear here shortly.";
        tag = "CONNECTING"; break;
    case DEMO_LIVE:
        title = "CarPlay connected";
        detail = "Use the touch screen to interact with CarPlay.\nUnplug your iPhone to return to this page.";
        tag = "CONNECTED"; break;
    case DEMO_CLOSING:
        title = "Returning to the launch page";
        detail = "Closing the previous connection...";
        tag = "PLEASE WAIT"; break;
    case DEMO_FAILED:
        title = "Unable to connect";
        detail = "Unplug your iPhone, unlock it, then reconnect.\nIf the problem continues, restart this display.";
        tag = "RECONNECT"; break;
    default:
        title = "Getting ready";
        detail = "Preparing the wired CarPlay receiver...";
        tag = "STARTING"; break;
    }
    lv_label_set_text(heading, title);
    lv_label_set_text(description, detail);
    lv_label_set_text(badge, tag);
    if (snapshot.stage >= DEMO_WAIT && snapshot.stage <= DEMO_CONNECT)
        lv_label_set_text_fmt(pin_label, "Pairing PIN: %s", snapshot.pin);
    else lv_label_set_text(pin_label, "");
    for (unsigned i = 0; i < 3; ++i) {
        bool active = (int)snapshot.stage >= DEMO_PAIR + (int)i && snapshot.stage <= DEMO_LIVE;
        lv_obj_set_style_bg_color(steps[i], lv_color_hex(active ? 0x38D9A9 : 0x2A3A50), 0);
    }
    /* Nothing animates or invalidates while live: video owns the visible layer. */
}

/* SDK calls this from lv_user_gui_init(), after display registration. */
void carplay_demo_launcher_init(void)
{
    if (status_lock != NULL) return;
    status_lock = rt_mutex_create("cpdemo", RT_IPC_FLAG_PRIO);
    if (status_lock == NULL) { rt_kprintf("[carplay-demo] no status mutex\n"); return; }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1220), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    int width = lv_display_get_horizontal_resolution(NULL);
    int height = lv_display_get_vertical_resolution(NULL);
    label(screen, "WIRED CARPLAY", 56, 38, width - 112, 0xE7EEF8, true);
    label(screen, "Connect. Trust. Enjoy.", 56, 78, width - 112, 0x91A7C0, false);
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_pos(card, 56, 134);
    lv_obj_set_size(card, width - 112, height - 238);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x142238), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A3A50), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_pad_all(card, 30, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    badge = label(card, "STARTING", 0, 0, width - 240, 0x38D9A9, false);
    heading = label(card, "Getting ready", 0, 48, width - 240, 0xFFFFFF, true);
    description = label(card, "Preparing the wired CarPlay receiver...", 0, 98,
                        width - 240, 0xB3C4DA, false);
    lv_obj_set_style_text_line_space(description, 10, 0);
    pin_label = label(card, "", 0, 170, width - 240, 0x91A7C0, false);
    for (unsigned i = 0; i < 3; ++i) {
        steps[i] = lv_obj_create(screen);
        lv_obj_set_size(steps[i], (width - 144) / 3, 4);
        lv_obj_set_pos(steps[i], 56 + (int)i * ((width - 144) / 3 + 16), height - 76);
        lv_obj_set_style_border_width(steps[i], 0, 0);
        lv_obj_set_style_bg_color(steps[i], lv_color_hex(0x2A3A50), 0);
    }
    label(screen, "01  Connect USB", 56, height - 57, 250, 0x91A7C0, false);
    label(screen, "02  Trust your display", width / 3 + 32, height - 57, 280, 0x91A7C0, false);
    label(screen, "03  Touch to explore", width * 2 / 3 + 8, height - 57, 280, 0x91A7C0, false);
    lv_timer_create(update_page, 250, NULL);
    rt_thread_t worker = rt_thread_create("cpdemo", demo_worker, NULL,
                                         24U * 1024U, 21, 10);
    if (worker == NULL || rt_thread_startup(worker) != RT_EOK) {
        if (worker != NULL) rt_thread_delete(worker);
        publish(DEMO_FAILED, RTCP_ENOMEM);
    }
}
