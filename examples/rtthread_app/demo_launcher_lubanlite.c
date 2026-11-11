/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Interactive LVGL launch page + one owner for the automatic wired demo.
 * LVGL objects are touched only by the LVGL thread. USB, TLS and shutdown
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

typedef enum {
    DEMO_BOOT, DEMO_WAIT, DEMO_PAIR, DEMO_NETWORK, DEMO_CONNECT,
    DEMO_LIVE, DEMO_CLOSING, DEMO_FAILED
} demo_stage_t;

typedef struct {
    demo_stage_t stage;
    int error;
    char pin[16];
    bool usb;
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
    static const char *names[] = {"Starting", "Waiting for USB", "USB pairing", "IPv6 setup",
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
    carplay_video_sink_stats(NULL, &sample.frames, &sample.drops, &sample.errors);
    carplay_app_debug_counters(&sample.received, &sample.hid_sent, &sample.hid_failed);
    carplay_app_performance_report();
    sample.trust = carplay_usb_trust_cache_status();
    sample.storage_error = carplay_app_pairing_storage_error();
    (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
    status.usb = sample.usb;
    memcpy(status.ipv6, sample.ipv6, sizeof(status.ipv6));
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
    (void)carplay_app_stop();
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
        if (result == RTCP_OK) result = carplay_pair_file_forget("/data", name);
        int start = carplay_app_autostart_wired();
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
    ready = !manager_open && file_action == FILE_NONE;
    if (ready) {
        status.previous = status.stage; status.stage = DEMO_PAIR;
        status.storage_error = 0;
    }
    (void)rt_mutex_release(status_lock);
    return ready;
}

static void demo_worker(void *argument)
{
    (void)argument;
    int ret = carplay_usb_open();
    if (ret != RTCP_OK) { publish(DEMO_FAILED, ret); return; }

    for (;;) {
        ret = carplay_app_is_running() ? RTCP_OK : carplay_app_autostart_wired();
        if (ret != RTCP_OK) {
            publish(DEMO_FAILED, ret);
            while (!idle_actions()) rt_thread_mdelay(200);
            continue;
        }
        (void)rt_mutex_take(status_lock, RT_WAITING_FOREVER);
        snprintf(status.pin, sizeof(status.pin), "%s", carplay_app_pairing_code());
        (void)rt_mutex_release(status_lock);
        publish(DEMO_WAIT, 0);
        for (;;) {
            bool retry = idle_actions();
            if (retry) {
                int start = carplay_app_is_running() ? RTCP_OK : carplay_app_autostart_wired();
                publish(start == RTCP_OK ? DEMO_WAIT : DEMO_FAILED, start);
            }
            sample_debug();
            if (carplay_app_is_running() && carplay_usb_attached() && claim_connection()) break;
            rt_thread_mdelay(200);
        }

        publish(DEMO_PAIR, 0);
        ret = carplay_usb_pair();
        if (ret == RTCP_OK) {
            publish(DEMO_NETWORK, 0);
            ret = RTCP_ETIMEDOUT;
            for (unsigned wait = 0; wait < 15000U && carplay_usb_attached(); wait += 100U) {
                carplay_app_wired_info_t endpoint;
                if (carplay_app_wired_info(&endpoint) == RTCP_OK) { ret = RTCP_OK; break; }
                sample_debug(); rt_thread_mdelay(100);
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
                sample_debug();
                if (carplay_app_pairing_storage_error() != RTCP_OK) { ret = RTCP_EIO; break; }
                unsigned frames = 0;
                carplay_video_sink_stats(NULL, &frames, NULL, NULL);
                if (frames != 0U) { ret = RTCP_OK; break; }
                rt_thread_mdelay(200);
            }
        }
        if (ret == RTCP_OK) {
            publish(DEMO_LIVE, 0);
            while (carplay_usb_attached() && carplay_usb_iap2_running() &&
                   carplay_app_is_running()) { sample_debug(); rt_thread_mdelay(250); }
        }
        sample_debug();
        close_session();
        if (carplay_usb_attached()) {
            /* One attempt per attachment: never spin pairing/key generation
             * against a rejected trust dialog or an incompatible phone. */
            int failed = ret != RTCP_OK ? ret : RTCP_EIO;
            int restart = carplay_app_autostart_wired();
            publish(DEMO_FAILED, restart != RTCP_OK ? restart : failed);
            while (carplay_usb_attached()) {
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
        !strcmp(name, "rt-carplay-pairings.bin") ?
        "AirPlay controllers will need to pair again." :
        "This iPhone may ask you to tap Trust again.");
    lv_obj_remove_flag(confirm_panel, LV_OBJ_FLAG_HIDDEN);
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
    demo_status_t snapshot;
    if (rt_mutex_take(status_lock, RT_WAITING_NO) != RT_EOK) return;
    snapshot = status;
    (void)rt_mutex_release(status_lock);
    if (snapshot.stage == DEMO_LIVE && (int)snapshot.stage == last_stage) return;
    static uint32_t last_debug_ms;
    uint32_t now = (uint32_t)rt_tick_get_millisecond();
    if (debug_label && ((int)snapshot.stage != last_stage || now - last_debug_ms >= 1000U)) {
        const char *trust = snapshot.trust == 2 ? "REUSED" : snapshot.trust == 3 ? "SAVED" :
                            snapshot.trust == 1 ? "NEW" : snapshot.trust < 0 ? "I/O ERROR" : "UNKNOWN";
        lv_label_set_text_fmt(debug_label,
            "Stage: %s\nPrevious: %s\nUSB: %s\nIPv6: %s\nTrust record: %s\nAirPlay storage: %s (%d)\nRX / displayed: %u / %u\nDrops / decode errors: %u / %u\nHID sent / failed: %u / %u\nLast error: %d",
            stage_name(snapshot.stage), stage_name(snapshot.previous),
            snapshot.usb ? "ATTACHED" : "DETACHED", snapshot.ipv6[0] ? snapshot.ipv6 : "waiting",
            trust, snapshot.storage_error ? "FAILED" : "OK", snapshot.storage_error,
            snapshot.received, snapshot.frames, snapshot.drops, snapshot.errors,
            snapshot.hid_sent, snapshot.hid_failed, snapshot.last_error);
        last_debug_ms = now;
    }
    if ((int)snapshot.stage == last_stage) return;
    last_stage = (int)snapshot.stage;
    if (snapshot.stage == DEMO_WAIT || snapshot.stage == DEMO_FAILED)
        lv_obj_remove_state(manage_button, LV_STATE_DISABLED);
    else lv_obj_add_state(manage_button, LV_STATE_DISABLED);
    if (snapshot.stage == DEMO_FAILED) lv_obj_remove_state(retry_button, LV_STATE_DISABLED);
    else lv_obj_add_state(retry_button, LV_STATE_DISABLED);
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
        detail = snapshot.storage_error ?
            "AirPlay could not save its pairing record.\nCheck storage diagnostics, then reconnect your iPhone." :
            "Unplug your iPhone, unlock it, then reconnect.\nSee connection diagnostics for the last error.";
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
    label(screen, "01  Connect USB", 56, height - 57, 250, 0x91A7C0, false);
    label(screen, "02  Trust your display", width / 3 + 32, height - 57, 280, 0x91A7C0, false);
    label(screen, "03  Touch to explore", width * 2 / 3 + 8, height - 57, 280, 0x91A7C0, false);
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
    confirm_panel = lv_obj_create(file_panel);
    lv_obj_set_pos(confirm_panel, 40, 50); lv_obj_set_size(confirm_panel, width - 180, height - 180);
    lv_obj_remove_flag(confirm_panel, LV_OBJ_FLAG_SCROLLABLE);
    confirm_text = label(confirm_panel, "", 10, 10, width - 240, 0xFFFFFF, false);
    button(confirm_panel, "Cancel", 10, height - 285, 160, cancel_forget);
    button(confirm_panel, "Forget record", 200, height - 285, 180, confirm_forget);
    lv_obj_add_flag(confirm_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(file_panel, LV_OBJ_FLAG_HIDDEN);
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
