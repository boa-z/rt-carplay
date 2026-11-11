/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Interactive LVGL launch page + one owner for USB and wireless sessions.
 * LVGL objects are touched only by the LVGL thread. Radio, USB, TLS and shutdown
 * run on a separate worker. The launcher retains exclusive panel ownership across receiver restarts. */
#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "carplay_app.h"
#include "carplay_usb.h"
#include "video_sink_lubanlite.h"
#include "rtcp/rtcp.h"
#include "rtcp/input.h"
#include "pairing_manager.h"
#include "touch_panel_lubanlite.h"
#include "iap2_link_lubanlite.h"
#include "rtcp_lubanlite.h"

#if defined(RTCARPLAY_IAP2) && defined(LPKG_RT_CARPLAY_LUBANLITE_WIFI)
#define DEMO_HAS_WIRELESS 1
#else
#define DEMO_HAS_WIRELESS 0
#endif

typedef enum {
    DEMO_BOOT, DEMO_WAIT, DEMO_PAIR, DEMO_NETWORK, DEMO_CONNECT,
    DEMO_LIVE, DEMO_CLOSING, DEMO_FAILED
} demo_stage_t;

typedef struct {
    demo_stage_t stage;
    int error;
    char pin[16];
    bool usb;
    bool wireless, bt_ready, bt_connected, bt_identified, bt_authenticated;
    char ssid[33];
    rtcp_bt_pairing_status_t pairing;
    char ipv6[64];
    unsigned received, frames, drops, errors, hid_sent, hid_failed;
    int trust, storage_error;
    demo_stage_t previous;
    int last_error;
} demo_status_t;

static rt_mutex_t status_lock;
static demo_status_t status;
static lv_obj_t *heading, *description, *pin_label, *badge, *debug_label;
static lv_obj_t *steps[3];
static lv_obj_t *step_labels[3], *mode_buttons[2];
static bool requested_wireless;
static uint32_t pairing_reply_request;
static bool pairing_reply_pending, pairing_reply_accept;
static lv_obj_t *bt_panel, *bt_text, *bt_accept;
static uint32_t visible_pairing_request;

/* The worker owns files and sessions; the LVGL thread only posts requests. */
typedef enum { FILE_NONE, FILE_SCAN, FILE_DELETE, FILE_RETRY } file_action_t;
static file_action_t file_action;
static bool manager_open, files_more;
static unsigned file_count, files_revision;
static int files_result;
static carplay_pair_file_t pair_files[CARPLAY_PAIR_FILES_MAX];
static char delete_name[CARPLAY_PAIR_FILENAME_MAX];
static lv_obj_t *manage_button, *retry_button, *file_panel, *file_list, *file_note;
static lv_obj_t *confirm_panel, *confirm_text;
static char selected_file[CARPLAY_PAIR_FILENAME_MAX];
typedef struct { uint16_t x, y; bool down; } demo_pointer_t;
static demo_pointer_t pointer_queue[32], pointer_state;
static unsigned pointer_head, pointer_tail;
static bool pointer_active;
static uint16_t pointer_id;

static bool mode_changed(void)
{
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    bool changed = requested_wireless != status.wireless;
    (void)rt_mutex_release(status_lock);
    return changed;
}

static int start_receiver(void)
{
    return status.wireless ? carplay_app_autostart() : carplay_app_autostart_wired();
}

/* Called only by the existing GT911 reader. No LVGL calls here. */
bool carplay_demo_touch(uint8_t phase, uint16_t id, uint16_t x, uint16_t y)
{
    unsigned frames = 0;
    carplay_video_sink_stats(NULL, &frames, NULL, NULL);
    if (frames && carplay_video_sink_is_running() && carplay_app_is_running()) {
        if (status_lock) {
            (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
            pointer_head = pointer_tail = 0; pointer_state.down = false; pointer_active = false;
            (void)rt_mutex_release(status_lock);
        }
        return false;
    }
    if (!status_lock) return true;
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    if (!pointer_active && phase == RTCP_TOUCH_DOWN) {
        pointer_active = true; pointer_id = id;
    }
    if (!pointer_active || pointer_id != id) {
        (void)rt_mutex_release(status_lock); return true;
    }
    if (phase == RTCP_TOUCH_UP) pointer_active = false;
    demo_pointer_t point = { x, y, phase != RTCP_TOUCH_UP };
    unsigned next = (pointer_head + 1U) % 32U;
    if (next == pointer_tail) {
        /* Retain the latest state, including release, if GUI is delayed. */
        pointer_tail = pointer_head;
    }
    pointer_queue[pointer_head] = point;
    pointer_head = next;
    (void)rt_mutex_release(status_lock);
    return true;
}

static void launcher_panel_touch(uint8_t phase, uint16_t id, uint16_t x,
                                  uint16_t y, void *context)
{
    (void)context;
    if (!carplay_demo_touch(phase, id, x, y)) {
        /* Closing publishes its stage under this lock before destroying the
         * app queue. Keep a report already in flight from racing teardown. */
        (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
        if (status.stage == DEMO_CONNECT || status.stage == DEMO_LIVE)
            (void)carplay_app_touch(phase, id, x, y);
        (void)rt_mutex_release(status_lock);
    }
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    if (pointer_tail != pointer_head) {
        pointer_state = pointer_queue[pointer_tail];
        pointer_tail = (pointer_tail + 1U) % 32U;
    }
    data->point.x = pointer_state.x; data->point.y = pointer_state.y;
    data->state = pointer_state.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = pointer_tail != pointer_head;
    (void)rt_mutex_release(status_lock);
}

static const char *stage_name(demo_stage_t stage)
{
    static const char *names[] = {"Starting", "Waiting for phone", "iAP2 pairing", "Network setup",
                                  "AirPlay pairing", "Video active", "Closing", "Failed"};
    return stage <= DEMO_FAILED ? names[stage] : "Unknown";
}

static void sample_debug(void)
{
    demo_status_t sample = {0};
    sample.usb = carplay_usb_attached();
    carplay_app_wired_info_t endpoint;
    if (carplay_app_is_running() && carplay_app_wired_info(&endpoint) == RTCP_OK)
        snprintf(sample.ipv6, sizeof(sample.ipv6), "%s", endpoint.ipv6);
#if DEMO_HAS_WIRELESS
    if (status.wireless) {
        carplay_app_iap2_info_t info;
        if (carplay_app_iap2_info(&info) == RTCP_OK) {
            snprintf(sample.ssid, sizeof(sample.ssid), "%s", info.ssid);
            snprintf(sample.ipv6, sizeof(sample.ipv6), "%u.%u.%u.%u",
                     info.ipv4[0], info.ipv4[1], info.ipv4[2], info.ipv4[3]);
        }
        carplay_iap2_status_t bt;
        carplay_iap2_get_status(&bt);
        sample.bt_ready = bt.listening; sample.bt_connected = bt.connected;
        sample.bt_identified = bt.identified; sample.bt_authenticated = bt.authenticated;
        rtcp_bt_lubanlite_pairing_status(&sample.pairing);
    }
#endif
    carplay_video_sink_stats(NULL, &sample.frames, &sample.drops, &sample.errors);
    carplay_app_debug_counters(&sample.received, &sample.hid_sent, &sample.hid_failed);
    carplay_app_performance_report();
    sample.trust = carplay_usb_trust_cache_status();
    sample.storage_error = carplay_app_pairing_storage_error();
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    status.usb = sample.usb;
    memcpy(status.ipv6, sample.ipv6, sizeof(status.ipv6));
    memcpy(status.ssid, sample.ssid, sizeof(status.ssid));
    status.bt_ready = sample.bt_ready; status.bt_connected = sample.bt_connected;
    status.bt_identified = sample.bt_identified; status.bt_authenticated = sample.bt_authenticated;
    status.pairing = sample.pairing;
    status.received = sample.received; status.frames = sample.frames;
    status.drops = sample.drops; status.errors = sample.errors;
    status.hid_sent = sample.hid_sent; status.hid_failed = sample.hid_failed;
    status.trust = sample.trust;
    if (sample.storage_error) status.storage_error = sample.storage_error;
    (void)rt_mutex_release(status_lock);
}

static void publish(demo_stage_t stage, int error)
{
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    if (stage != DEMO_FAILED || status.stage != DEMO_CLOSING) status.previous = status.stage;
    if (stage == DEMO_WAIT) status.storage_error = 0;
    status.stage = stage;
    status.error = error;
    if (error) status.last_error = error;
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
#if DEMO_HAS_WIRELESS
    /* An asynchronous BT controller startup may exceed one stop timeout.
     * Keep its borrowed receiver state alive until the owner really exits. */
    while (carplay_iap2_stop() != RTCP_OK) rt_thread_mdelay(250);
#endif
    while (carplay_app_stop() != RTCP_OK) rt_thread_mdelay(250);
}

/* Called only with no active USB session. The launcher's single panel
 * reader remains usable even if receiver startup/storage loading failed. */
static bool idle_actions(void)
{
    char name[CARPLAY_PAIR_FILENAME_MAX];
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    file_action_t action = file_action;
    file_action = FILE_NONE;
    snprintf(name, sizeof(name), "%s", delete_name);
    (void)rt_mutex_release(status_lock);
    if (action == FILE_RETRY) return true;
    if (action == FILE_NONE) return false;
    int result = RTCP_OK;
    if (action == FILE_DELETE) {
        /* USB is quiescent here. Restart reloads the AirPlay controller table
         * and prevents an old in-memory copy from rewriting a deleted file. */
        result = carplay_app_stop();
        if (result == RTCP_OK) {
#if DEMO_HAS_WIRELESS
            if (!strcmp(name, "Bluetooth bonds")) result = rtcp_bt_lubanlite_forget_bonds();
            else
#endif
                result = carplay_pair_file_forget("/data", name);
        }
        int start = status.wireless ? RTCP_OK : start_receiver();
        if (start != RTCP_OK) { publish(DEMO_FAILED, start); result = start; }
    }
    carplay_pair_file_t files[CARPLAY_PAIR_FILES_MAX];
    unsigned count = 0; bool more = false;
    int scan = carplay_pair_files_scan("/data", files, CARPLAY_PAIR_FILES_MAX, &count, &more);
    if (result == RTCP_OK) result = scan;
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    memcpy(pair_files, files, count * sizeof(files[0]));
    file_count = count; files_more = more; files_result = result;
    ++files_revision;
    (void)rt_mutex_release(status_lock);
    return false;
}

/* Reserve the pairing transition while holding the same lock as UI actions. */
static bool claim_connection(void)
{
    bool ready;
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    ready = !manager_open && file_action == FILE_NONE && !requested_wireless;
    if (ready) {
        status.previous = status.stage; status.stage = DEMO_PAIR;
        status.storage_error = 0;
    }
    (void)rt_mutex_release(status_lock);
    return ready;
}

#if DEMO_HAS_WIRELESS
static int wireless_connection(void)
{
    int result = carplay_iap2_start();
    if (result != RTCP_OK) return result;
    bool handover = false, live = false, connecting = false;
    uint32_t started = rt_tick_get_millisecond(), pairing_started = 0;
    while (!mode_changed() && carplay_app_is_running()) {
        (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
        bool managing = manager_open;
        bool reply = pairing_reply_pending, accept = pairing_reply_accept;
        uint32_t request = pairing_reply_request;
        pairing_reply_pending = false;
        (void)rt_mutex_release(status_lock);
        if (reply) (void)rtcp_bt_lubanlite_pairing_reply(request, accept);
        if (managing) {
            /* Freeze incoming BT/AirPlay pairing before reading/deleting
             * records, and keep it frozen until the modal is closed. */
            close_session();
            for (;;) {
                (void)idle_actions();
                (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
                managing = manager_open || file_action != FILE_NONE;
                (void)rt_mutex_release(status_lock);
                if (!managing || mode_changed()) return RTCP_OK;
                rt_thread_mdelay(100);
            }
        }
        carplay_iap2_status_t bt;
        carplay_iap2_get_status(&bt);
        if (bt.error) return bt.error;
        if (!bt.running) return RTCP_EIO;
        if (!bt.listening && (uint32_t)(rt_tick_get_millisecond() - started) > 60000U)
            return RTCP_ETIMEDOUT;
        carplay_app_iap2_info_t info;
        if (carplay_app_iap2_info(&info) != RTCP_OK) return RTCP_ESTATE;
        if (!connecting && (bt.connected || info.airplay_connected)) {
            connecting = true; pairing_started = rt_tick_get_millisecond();
        }
        handover |= bt.authenticated || info.airplay_connected;
        unsigned frames = 0;
        carplay_video_sink_stats(NULL, &frames, NULL, NULL);
        if (carplay_app_pairing_storage_error()) return RTCP_EIO;
        if (live && !info.airplay_connected) return RTCP_OK;
        demo_stage_t stage = frames && info.airplay_connected ? DEMO_LIVE :
            info.airplay_connected ? DEMO_CONNECT : handover ? DEMO_NETWORK :
            bt.connected ? DEMO_PAIR : bt.listening ? DEMO_WAIT : DEMO_BOOT;
        if (stage != status.stage) publish(stage, 0);
        live |= stage == DEMO_LIVE;
        if (connecting && !live &&
            (uint32_t)(rt_tick_get_millisecond() - pairing_started) > 90000U)
            return RTCP_ETIMEDOUT;
        sample_debug();
        rt_thread_mdelay(200);
    }
    return mode_changed() ? RTCP_OK : RTCP_EIO;
}
#endif

static void demo_worker(void *argument)
{
    (void)argument;
    int ret = carplay_usb_open();
    if (ret != RTCP_OK) { publish(DEMO_FAILED, ret); return; }

    for (;;) {
        if (mode_changed()) {
            close_session();
            (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
            status.wireless = requested_wireless;
            status.stage = DEMO_BOOT;
            status.last_error = status.storage_error = 0;
            (void)rt_mutex_release(status_lock);
        }
        ret = carplay_app_is_running() ? RTCP_OK : start_receiver();
        if (ret != RTCP_OK) {
            publish(DEMO_FAILED, ret);
            while (!mode_changed() && !idle_actions()) rt_thread_mdelay(200);
            continue;
        }
        (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
        snprintf(status.pin, sizeof(status.pin), "%s", carplay_app_pairing_code());
        (void)rt_mutex_release(status_lock);
#if DEMO_HAS_WIRELESS
        if (status.wireless) {
            ret = wireless_connection();
            sample_debug();
            close_session();
            if (ret != RTCP_OK && !mode_changed()) {
                publish(DEMO_FAILED, ret);
                while (!mode_changed() && !idle_actions()) rt_thread_mdelay(200);
            }
            continue;
        }
#endif
        publish(DEMO_WAIT, 0);
        for (;;) {
            if (mode_changed()) break;
            bool retry = idle_actions();
            if (retry) {
                int start = carplay_app_is_running() ? RTCP_OK : start_receiver();
                publish(start == RTCP_OK ? DEMO_WAIT : DEMO_FAILED, start);
            }
            sample_debug();
            if (carplay_app_is_running() && carplay_usb_attached() && claim_connection()) break;
            rt_thread_mdelay(200);
        }
        if (mode_changed()) continue;

        publish(DEMO_PAIR, 0);
        ret = carplay_usb_pair();
        if (ret == RTCP_OK) {
            publish(DEMO_NETWORK, 0);
            ret = RTCP_ETIMEDOUT;
            for (unsigned wait = 0; wait < 15000U && carplay_usb_attached() && !mode_changed(); wait += 100U) {
                carplay_app_wired_info_t endpoint;
                if (carplay_app_wired_info(&endpoint) == RTCP_OK) { ret = RTCP_OK; break; }
                sample_debug(); rt_thread_mdelay(100);
            }
        }
        if (ret == RTCP_OK && !mode_changed()) {
            publish(DEMO_CONNECT, 0);
            ret = carplay_usb_start_iap2();
        }
        if (ret == RTCP_OK) {
            ret = RTCP_ETIMEDOUT;
            for (unsigned wait = 0; wait < 90000U && carplay_usb_attached() &&
                                      carplay_usb_iap2_running() && !mode_changed(); wait += 200U) {
                sample_debug();
                if (carplay_app_pairing_storage_error() != RTCP_OK) { ret = RTCP_EIO; break; }
                unsigned frames = 0;
                carplay_video_sink_stats(NULL, &frames, NULL, NULL);
                if (frames != 0U) { ret = RTCP_OK; break; }
                rt_thread_mdelay(200);
            }
        }
        if (ret == RTCP_OK && !mode_changed()) {
            publish(DEMO_LIVE, 0);
            while (carplay_usb_attached() && carplay_usb_iap2_running() &&
                   carplay_app_is_running() && !mode_changed()) { sample_debug(); rt_thread_mdelay(250); }
        }
        sample_debug();
        close_session();
        if (mode_changed()) continue;
        if (carplay_usb_attached()) {
            /* One attempt per attachment: never spin pairing/key generation
             * against a rejected trust dialog or an incompatible phone. */
            int failed = ret != RTCP_OK ? ret : RTCP_EIO;
            int restart = start_receiver();
            publish(DEMO_FAILED, restart != RTCP_OK ? restart : failed);
            while (carplay_usb_attached() && !mode_changed()) {
                if (idle_actions()) break;
                rt_thread_mdelay(200);
            }
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

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y,
                         int width, lv_event_cb_t callback)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, width, 44);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x245C80), 0);
    lv_obj_t *caption = lv_label_create(obj);
    lv_label_set_text(caption, text); lv_obj_center(caption);
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, NULL);
    return obj;
}

static void files_back(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(file_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(confirm_panel, LV_OBJ_FLAG_HIDDEN);
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    manager_open = false;
    (void)rt_mutex_release(status_lock);
}
static void files_refresh(lv_event_t *event)
{
    (void)event;
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    if (file_action == FILE_NONE) file_action = FILE_SCAN;
    (void)rt_mutex_release(status_lock);
    lv_label_set_text(file_note, "Reading pairing files...");
}
static void files_open(lv_event_t *event)
{
    (void)event;
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    bool idle = status.stage == DEMO_WAIT || status.stage == DEMO_FAILED;
    if (idle) { manager_open = true; file_action = FILE_SCAN; }
    (void)rt_mutex_release(status_lock);
    if (!idle) return;
    lv_obj_remove_flag(file_panel, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(file_note, "Reading pairing files...");
}
static void retry_connection(lv_event_t *event)
{
    (void)event;
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    if (status.stage == DEMO_FAILED && !manager_open && file_action == FILE_NONE)
        file_action = FILE_RETRY;
    (void)rt_mutex_release(status_lock);
}
static void select_mode(lv_event_t *event)
{
    bool wireless = lv_event_get_target(event) == mode_buttons[1];
    if (wireless && !DEMO_HAS_WIRELESS) return;
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    if (!manager_open && file_action == FILE_NONE &&
        status.stage != DEMO_CLOSING && status.stage != DEMO_LIVE)
        requested_wireless = wireless;
    (void)rt_mutex_release(status_lock);
}
static void answer_bluetooth(lv_event_t *event)
{
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    pairing_reply_request = visible_pairing_request;
    pairing_reply_accept = lv_event_get_target(event) == bt_accept;
    pairing_reply_pending = true;
    (void)rt_mutex_release(status_lock);
    lv_obj_add_flag(bt_panel, LV_OBJ_FLAG_HIDDEN);
}
static void cancel_forget(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(confirm_panel, LV_OBJ_FLAG_HIDDEN);
}
static void confirm_forget(lv_event_t *event)
{
    (void)event;
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    if (manager_open && file_action == FILE_NONE) {
        snprintf(delete_name, sizeof(delete_name), "%s", selected_file);
        file_action = FILE_DELETE;
    }
    (void)rt_mutex_release(status_lock);
    lv_obj_add_flag(confirm_panel, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(file_note, "Forgetting selected record and restarting receiver...");
}
static void select_file(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    const char *name = lv_obj_get_user_data(target);
    snprintf(selected_file, sizeof(selected_file), "%s", name);
    lv_label_set_text_fmt(confirm_text,
        "Forget this pairing record?\n\n%s\n\n%s\nDevice identity will be kept.", name,
        !strcmp(name, "Bluetooth bonds") ?
        "Forget this display in iPhone Bluetooth settings, then pair again." :
        !strcmp(name, "rt-carplay-pairings.bin") ?
        "AirPlay controllers will need to pair again." :
        "This iPhone may ask you to tap Trust again.");
    lv_obj_remove_flag(confirm_panel, LV_OBJ_FLAG_HIDDEN);
}
static void select_bluetooth_bonds(lv_event_t *event)
{
    lv_obj_set_user_data(lv_event_get_target(event), "Bluetooth bonds");
    select_file(event);
}
static void update_files(void)
{
    static unsigned revision;
    /* UI owns this copy: button user-data stays stable until list rebuilt. */
    static carplay_pair_file_t visible[CARPLAY_PAIR_FILES_MAX];
    unsigned count; bool more; int result;
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    if (revision == files_revision || !manager_open) {
        (void)rt_mutex_release(status_lock); return;
    }
    revision = files_revision; count = file_count; more = files_more; result = files_result;
    memcpy(visible, pair_files, count * sizeof(visible[0]));
    (void)rt_mutex_release(status_lock);
    lv_obj_clean(file_list);
    for (unsigned i = 0; i < count; ++i) {
        char text[140];
        snprintf(text, sizeof(text), "%s (%u bytes)", visible[i].name, (unsigned)visible[i].bytes);
        lv_obj_t *row = button(file_list, text, 0, (int)i * 50, 810, select_file);
        lv_obj_set_user_data(row, visible[i].name);
    }
    lv_label_set_text_fmt(file_note,
        "Records: %u%s | Result: %d\nTap a record to forget it. Identity keys are protected.",
        count, more ? "+ (refresh after deletion for more)" : "", result);
}

static void update_page(lv_timer_t *timer)
{
    (void)timer;
    update_files();
    static int last_stage = -1;
    static int last_mode = -1;
    demo_status_t snapshot;
    if (rt_mutex_take(status_lock, RT_WAITING_NO) != RT_EOK) return;
    snapshot = status;
    (void)rt_mutex_release(status_lock);
    if (snapshot.wireless && snapshot.pairing.kind && snapshot.stage != DEMO_CLOSING &&
        snapshot.stage != DEMO_FAILED) {
        if (visible_pairing_request != snapshot.pairing.request) {
            visible_pairing_request = snapshot.pairing.request;
            if (snapshot.pairing.kind == RTCP_BT_PAIR_CONSENT)
                lv_label_set_text(bt_text, "Allow this Bluetooth pairing?\nConfirm only if you just started pairing on your iPhone.");
            else lv_label_set_text_fmt(bt_text,
                snapshot.pairing.kind == RTCP_BT_PAIR_COMPARE ?
                "Bluetooth pairing: %06u\n\nCheck that your iPhone shows the same number.\nThen confirm on both screens." :
                "Bluetooth pairing: %06u\n\nEnter this number on your iPhone.", snapshot.pairing.passkey);
            if (snapshot.pairing.kind == RTCP_BT_PAIR_DISPLAY) lv_obj_add_state(bt_accept, LV_STATE_DISABLED);
            else lv_obj_remove_state(bt_accept, LV_STATE_DISABLED);
            lv_obj_remove_flag(bt_panel, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        visible_pairing_request = 0;
        lv_obj_add_flag(bt_panel, LV_OBJ_FLAG_HIDDEN);
    }
    bool mode_update = last_mode != (int)snapshot.wireless;
    if (snapshot.stage == DEMO_LIVE && (int)snapshot.stage == last_stage && !mode_update) return;
    static uint32_t last_debug_ms;
    uint32_t now = (uint32_t)rt_tick_get_millisecond();
    if (debug_label && ((int)snapshot.stage != last_stage || now - last_debug_ms >= 1000U)) {
        const char *trust = snapshot.trust == 2 ? "REUSED" : snapshot.trust == 3 ? "SAVED" :
                            snapshot.trust == 1 ? "NEW" : snapshot.trust < 0 ? "I/O ERROR" : "UNKNOWN";
        lv_label_set_text_fmt(debug_label,
            "Mode: %s\nStage: %s\nUSB: %s\nIP: %s\nTrust record: %s\nAirPlay storage: %s (%d)\nRX / displayed: %u / %u\nDrops / decode errors: %u / %u\nHID sent / failed: %u / %u\nLast error: %d",
            snapshot.wireless ? "Wireless" : "USB",
            stage_name(snapshot.stage),
            snapshot.usb ? "ATTACHED" : "DETACHED", snapshot.ipv6[0] ? snapshot.ipv6 : "waiting",
            trust, snapshot.storage_error ? "FAILED" : "OK", snapshot.storage_error,
            snapshot.received, snapshot.frames, snapshot.drops, snapshot.errors,
            snapshot.hid_sent, snapshot.hid_failed, snapshot.last_error);
        last_debug_ms = now;
        if (snapshot.wireless)
            lv_label_set_text_fmt(pin_label,
                "Wi-Fi: %s\nBluetooth: %s | MFi: %s\nBT last HCI/security: %02x/%u | Bonds %u/%u: %s",
                snapshot.ssid[0] ? snapshot.ssid : "starting",
                snapshot.bt_connected ? "connected" : snapshot.bt_ready ? "discoverable" : "starting",
                snapshot.bt_authenticated ? "authenticated" : snapshot.bt_identified ? "identified" : "waiting",
                snapshot.pairing.hci_error, snapshot.pairing.security_error,
                snapshot.pairing.bond_count, snapshot.pairing.bond_capacity,
                snapshot.pairing.storage_error ? "I/O ERROR" : snapshot.pairing.persistent ? "storage enabled" : "RAM only");
    }
    if ((int)snapshot.stage == last_stage && !mode_update) return;
    last_stage = (int)snapshot.stage;
    last_mode = snapshot.wireless;
    lv_obj_set_style_bg_color(mode_buttons[0], lv_color_hex(snapshot.wireless ? 0x245C80 : 0x287D68), 0);
    lv_obj_set_style_bg_color(mode_buttons[1], lv_color_hex(snapshot.wireless ? 0x287D68 : 0x245C80), 0);
    for (unsigned i = 0; i < 2; ++i) {
        if (snapshot.stage == DEMO_CLOSING || snapshot.stage == DEMO_LIVE || (i && !DEMO_HAS_WIRELESS))
            lv_obj_add_state(mode_buttons[i], LV_STATE_DISABLED);
        else lv_obj_remove_state(mode_buttons[i], LV_STATE_DISABLED);
    }
    lv_label_set_text(step_labels[0], snapshot.wireless ? "01  Pair Bluetooth" : "01  Connect USB");
    lv_label_set_text(step_labels[1], snapshot.wireless ? "02  Join CarPlay Wi-Fi" : "02  Trust your display");
    if (snapshot.stage == DEMO_WAIT || snapshot.stage == DEMO_FAILED)
        lv_obj_remove_state(manage_button, LV_STATE_DISABLED);
    else lv_obj_add_state(manage_button, LV_STATE_DISABLED);
    if (snapshot.stage == DEMO_FAILED) lv_obj_remove_state(retry_button, LV_STATE_DISABLED);
    else lv_obj_add_state(retry_button, LV_STATE_DISABLED);
    const char *title, *detail, *tag;
    switch (snapshot.stage) {
    case DEMO_WAIT:
        title = "Connect your iPhone";
        detail = snapshot.wireless ?
            "On your iPhone, open Settings > Bluetooth.\nPair with rt-carplay and allow CarPlay." :
            "Plug an unlocked iPhone into the USB port.\nCarPlay will connect automatically.";
        tag = "READY"; break;
    case DEMO_PAIR:
        title = snapshot.wireless ? "Pairing over Bluetooth" : "Trust this connection";
        detail = snapshot.wireless ?
            "Keep Bluetooth and Wi-Fi enabled on your iPhone.\nAccept pairing and CarPlay prompts on the phone." :
            "Keep your iPhone unlocked.\nTap Trust on the phone if asked. This may take a moment.";
        tag = "PAIRING"; break;
    case DEMO_NETWORK:
        title = "Preparing the connection";
        detail = snapshot.wireless ?
            "Authentication completed.\nWaiting for your iPhone to join CarPlay Wi-Fi..." :
            "Your iPhone is paired.\nSetting up the wired connection...";
        tag = "CONNECTING"; break;
    case DEMO_CONNECT:
        title = "Starting CarPlay";
        detail = "Allow CarPlay on your iPhone if asked.\nYour apps will appear here shortly.";
        tag = "CONNECTING"; break;
    case DEMO_LIVE:
        title = "CarPlay connected";
        detail = snapshot.wireless ?
            "Use the touch screen to interact with CarPlay.\nDisconnect CarPlay on your iPhone to return." :
            "Use the touch screen to interact with CarPlay.\nUnplug your iPhone to return to this page.";
        tag = "CONNECTED"; break;
    case DEMO_CLOSING:
        title = "Returning to the launch page";
        detail = "Closing the previous connection...";
        tag = "PLEASE WAIT"; break;
    case DEMO_FAILED:
        title = "Unable to connect";
        detail = snapshot.storage_error ?
            "AirPlay could not save its pairing record.\nCheck storage diagnostics, then reconnect your iPhone." :
            snapshot.wireless ?
            "Check Bluetooth and Wi-Fi on your iPhone.\nTap Retry, or choose USB. See diagnostics for the error." :
            "Unplug your iPhone, unlock it, then reconnect.\nSee connection diagnostics for the last error.";
        tag = "RECONNECT"; break;
    default:
        title = "Getting ready";
        detail = snapshot.wireless ? "Starting CarPlay Wi-Fi and Bluetooth..." :
                                    "Preparing the wired CarPlay receiver...";
        tag = "STARTING"; break;
    }
    lv_label_set_text(heading, title);
    lv_label_set_text(description, detail);
    lv_label_set_text(badge, tag);
    if (!snapshot.wireless && snapshot.stage >= DEMO_WAIT && snapshot.stage <= DEMO_CONNECT)
        lv_label_set_text_fmt(pin_label, "Pairing PIN: %s", snapshot.pin);
    else if (!snapshot.wireless) lv_label_set_text(pin_label, "");
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
    label(screen, "CARPLAY", 56, 38, width - 500, 0xE7EEF8, true);
    label(screen, "Choose USB or wireless to connect your iPhone.", 56, 78, width - 112, 0x91A7C0, false);
    mode_buttons[0] = button(screen, "USB", width - 410, 32, 160, select_mode);
    mode_buttons[1] = button(screen, DEMO_HAS_WIRELESS ? "Wireless" : "Wireless unavailable",
                             width - 234, 32, 178, select_mode);
    if (!DEMO_HAS_WIRELESS) lv_obj_add_state(mode_buttons[1], LV_STATE_DISABLED);
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_pos(card, 56, 134);
    lv_obj_set_size(card, width - 424, height - 238);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x142238), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A3A50), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_pad_all(card, 30, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    badge = label(card, "STARTING", 0, 0, width - 500, 0x38D9A9, false);
    heading = label(card, "Getting ready", 0, 48, width - 500, 0xFFFFFF, true);
    description = label(card, "Preparing the wired CarPlay receiver...", 0, 98,
                        width - 500, 0xB3C4DA, false);
    lv_obj_set_style_text_line_space(description, 10, 0);
    pin_label = label(card, "", 0, 170, width - 500, 0x91A7C0, false);
    lv_obj_t *debug_card = lv_obj_create(screen);
    lv_obj_set_pos(debug_card, width - 352, 134);
    lv_obj_set_size(debug_card, 296, height - 238);
    lv_obj_set_style_bg_color(debug_card, lv_color_hex(0x142238), 0);
    lv_obj_set_style_border_color(debug_card, lv_color_hex(0x2A3A50), 0);
    lv_obj_set_style_border_width(debug_card, 1, 0);
    lv_obj_set_style_radius(debug_card, 24, 0);
    lv_obj_set_style_pad_all(debug_card, 18, 0);
    lv_obj_remove_flag(debug_card, LV_OBJ_FLAG_SCROLLABLE);
    label(debug_card, "CONNECTION DIAGNOSTICS", 0, 0, 260, 0x38D9A9, false);
    debug_label = label(debug_card, "Waiting for diagnostics...", 0, 32, 260, 0xB3C4DA, false);
#if LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(debug_label, &lv_font_montserrat_12, 0);
#endif
    lv_obj_set_style_text_line_space(debug_label, 7, 0);
    for (unsigned i = 0; i < 3; ++i) {
        steps[i] = lv_obj_create(screen);
        lv_obj_set_size(steps[i], (width - 144) / 3, 4);
        lv_obj_set_pos(steps[i], 56 + (int)i * ((width - 144) / 3 + 16), height - 76);
        lv_obj_set_style_border_width(steps[i], 0, 0);
        lv_obj_set_style_bg_color(steps[i], lv_color_hex(0x2A3A50), 0);
    }
    step_labels[0] = label(screen, "01  Connect USB", 56, height - 57, 250, 0x91A7C0, false);
    step_labels[1] = label(screen, "02  Trust your display", width / 3 + 32, height - 57, 280, 0x91A7C0, false);
    step_labels[2] = label(screen, "03  Touch to explore", width * 2 / 3 + 8, height - 57, 280, 0x91A7C0, false);
    manage_button = button(screen, "Pair files", 56, height - 145, 180, files_open);
    retry_button = button(screen, "Retry connection", 252, height - 145, 200, retry_connection);
    lv_obj_add_state(manage_button, LV_STATE_DISABLED);
    lv_obj_add_state(retry_button, LV_STATE_DISABLED);
    file_panel = lv_obj_create(screen);
    lv_obj_set_pos(file_panel, 30, 25); lv_obj_set_size(file_panel, width - 60, height - 50);
    lv_obj_set_style_bg_color(file_panel, lv_color_hex(0x142238), 0);
    lv_obj_remove_flag(file_panel, LV_OBJ_FLAG_SCROLLABLE);
    label(file_panel, "PAIRING FILES", 10, 0, 600, 0xFFFFFF, true);
    file_note = label(file_panel, "", 10, 40, width - 110, 0xB3C4DA, false);
    file_list = lv_obj_create(file_panel);
    lv_obj_set_pos(file_list, 10, 100); lv_obj_set_size(file_list, width - 120, height - 260);
    lv_obj_set_style_pad_all(file_list, 8, 0);
    button(file_panel, "Refresh", 10, height - 145, 150, files_refresh);
    button(file_panel, "Back", 180, height - 145, 150, files_back);
#if DEMO_HAS_WIRELESS
    button(file_panel, "Forget Bluetooth pairs", 350, height - 145, 250, select_bluetooth_bonds);
#else
    (void)&select_bluetooth_bonds;
#endif
    confirm_panel = lv_obj_create(file_panel);
    lv_obj_set_pos(confirm_panel, 40, 50); lv_obj_set_size(confirm_panel, width - 180, height - 180);
    lv_obj_remove_flag(confirm_panel, LV_OBJ_FLAG_SCROLLABLE);
    confirm_text = label(confirm_panel, "", 10, 10, width - 240, 0xFFFFFF, false);
    button(confirm_panel, "Cancel", 10, height - 285, 160, cancel_forget);
    button(confirm_panel, "Forget record", 200, height - 285, 180, confirm_forget);
    lv_obj_add_flag(confirm_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(file_panel, LV_OBJ_FLAG_HIDDEN);
    bt_panel = lv_obj_create(screen);
    lv_obj_set_pos(bt_panel, 80, 120); lv_obj_set_size(bt_panel, width - 160, height - 240);
    lv_obj_remove_flag(bt_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bt_panel, lv_color_hex(0x142238), 0);
    bt_text = label(bt_panel, "", 20, 20, width - 240, 0xFFFFFF, true);
    bt_accept = button(bt_panel, "Numbers match / Allow", 20, height - 340, 260, answer_bluetooth);
    button(bt_panel, "Cancel pairing", 300, height - 340, 200, answer_bluetooth);
    lv_obj_add_flag(bt_panel, LV_OBJ_FLAG_HIDDEN);
    lv_indev_t *pointer = lv_indev_create();
    if (pointer) {
        lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(pointer, pointer_read);
    }
    lv_timer_create(update_page, 250, NULL);
    int touch = carplay_touch_panel_start((uint16_t)width, (uint16_t)height,
                                          launcher_panel_touch, NULL);
    if (touch != RTCP_OK) rt_kprintf("[carplay-demo] launch page touch error=%d\n", touch);
    rt_thread_t worker = rt_thread_create("cpdemo", demo_worker, NULL,
                                         24U * 1024U, 21, 10);
    if (worker == NULL || rt_thread_startup(worker) != RT_EOK) {
        if (worker != NULL) rt_thread_delete(worker);
        publish(DEMO_FAILED, RTCP_ENOMEM);
    }
}
