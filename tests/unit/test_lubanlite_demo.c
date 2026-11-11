/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Drive the actual automatic worker. UI compiles against SDK LVGL headers;
 * unused UI sections are removed, so this does not test framebuffer rendering. */
#include <assert.h>
#include <setjmp.h>
#include "../../examples/rtthread_app/demo_launcher_lubanlite.c"

static jmp_buf finished;
static unsigned mode, starts, pairs, closes, stops, net_checks, pictures, failed_waits;
static bool session_live, quiesced, receiver_running;
static unsigned reports;
uint32_t rt_tick_get_millisecond(void) { return 0; }
int carplay_usb_trust_cache_status(void) { return 0; }
int carplay_app_pairing_storage_error(void) { return mode == 3 && status.stage == DEMO_CONNECT ? RTCP_EIO : 0; }
void carplay_app_debug_counters(unsigned *r, unsigned *h, unsigned *e) { *r=*h=*e=0; }
int rt_kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int rt_mutex_take(rt_mutex_t m, int wait) { (void)m; (void)wait; return 0; }
int rt_mutex_release(rt_mutex_t m) { (void)m; return 0; }
void rt_thread_mdelay(int ms) {
    assert(ms > 0);
    if (status.stage == DEMO_FAILED) ++failed_waits;
}
int carplay_usb_open(void) { return 0; }
bool carplay_usb_attached(void) {
    if (status.stage == DEMO_LIVE) return false; /* unplug after first picture */
    if (status.stage == DEMO_FAILED) return failed_waits < 3;
    return true;
}
int carplay_usb_pair(void) {
    if ((mode == 1 || mode == 3) && pairs == 1) longjmp(finished, 1);
    assert(status.stage == DEMO_PAIR && starts == 1);
    ++pairs; session_live = true;
    return mode == 1 ? RTCP_EIO : RTCP_OK;
}
int carplay_usb_start_iap2(void) {
    assert(net_checks >= 2 && status.stage == DEMO_CONNECT);
    return 0;
}
bool carplay_usb_iap2_running(void) { return true; }
int carplay_usb_close_session(void) {
    ++closes;
    if (mode == 2 && closes == 1) return RTCP_ETIMEDOUT;
    session_live = false; quiesced = true; return 0;
}
int carplay_app_autostart_wired(void) {
    assert(!session_live);
    if (++starts == 2 && mode != 1 && mode != 3) longjmp(finished, 1);
    receiver_running = true;
    return 0;
}
const char *carplay_app_pairing_code(void) { return "3939"; }
int carplay_app_wired_info(carplay_app_wired_info_t *info) {
    (void)info;
    if (status.stage != DEMO_NETWORK) return RTCP_ENOENT;
    ++net_checks;
    return net_checks == 1 ? RTCP_ENOENT : RTCP_OK;
}
bool carplay_app_is_running(void) { return receiver_running; }
int carplay_app_stop(void) {
    assert(quiesced && !session_live && closes >= (mode == 2 ? 2U : 1U));
    ++stops; receiver_running = false; return 0;
}
void carplay_video_sink_stats(unsigned *n, unsigned *f, unsigned *d, unsigned *e) {
    (void)n; (void)d; (void)e;
    if (status.stage == DEMO_CONNECT) ++pictures;
    if (f) *f = pictures >= 2 ? 1 : 0;
    if (d) *d = 0;
    if (e) *e = 0;
}
bool carplay_video_sink_is_running(void) { return receiver_running; }
int carplay_app_touch(uint8_t phase, uint16_t id, uint16_t x, uint16_t y) {
    (void)phase; (void)id; (void)x; (void)y;
    assert(receiver_running && (status.stage == DEMO_LIVE || status.stage == DEMO_CONNECT));
    ++reports; return RTCP_OK;
}
int main(void) {
    for (mode = 0; mode < 4; ++mode) {
        starts = pairs = closes = stops = net_checks = pictures = failed_waits = 0;
        session_live = quiesced = receiver_running = false;
        memset(&status, 0, sizeof(status));
        if (setjmp(finished) == 0) demo_worker(NULL);
        assert(starts == 2 && pairs == 1 && stops == 1);
        assert(closes == (mode == 2 ? 2U : 1U));
        if (mode == 1) assert(failed_waits == 3 && net_checks == 0);
        else if (mode == 3) assert(failed_waits == 3 && pictures == 2);
        else assert(net_checks >= 2 && pictures >= 2);
    }
    /* Touch is delivered to LVGL before picture, and to CarPlay after picture. */
    status_lock = (void *)1; status.stage = DEMO_WAIT; pictures = 0;
    receiver_running = true;
    lv_indev_data_t point = {0};
    assert(carplay_demo_touch(RTCP_TOUCH_DOWN, 7, 100, 200));
    assert(carplay_demo_touch(RTCP_TOUCH_DOWN, 9, 800, 500)); /* second finger ignored */
    pointer_read(NULL,&point);
    assert(point.state == LV_INDEV_STATE_PRESSED && point.point.x == 100 && point.point.y == 200);
    assert(carplay_demo_touch(RTCP_TOUCH_UP, 7, 120, 220));
    pointer_read(NULL,&point);
    assert(point.state == LV_INDEV_STATE_RELEASED && point.point.x == 120);
    for (unsigned i=0;i<80;++i) assert(carplay_demo_touch(i ? RTCP_TOUCH_MOVE : RTCP_TOUCH_DOWN,7,i,i));
    assert(carplay_demo_touch(RTCP_TOUCH_UP,7,99,99));
    do { pointer_read(NULL,&point); } while (point.continue_reading);
    assert(point.state == LV_INDEV_STATE_RELEASED && point.point.x == 99);
    manager_open = true; assert(!claim_connection());
    manager_open = false; file_action = FILE_SCAN; assert(!claim_connection());
    file_action = FILE_NONE; assert(claim_connection() && status.stage == DEMO_PAIR);
    pictures = 2; status.stage = DEMO_LIVE;
    launcher_panel_touch(RTCP_TOUCH_DOWN,7,10,10,NULL);
    assert(reports==1);
    status.stage = DEMO_CLOSING;
    launcher_panel_touch(RTCP_TOUCH_MOVE,7,20,20,NULL);
    assert(reports==1); /* closing gates reports before queue teardown */
    assert(!carplay_demo_touch(RTCP_TOUCH_DOWN,7,10,10));
    receiver_running = false;
    assert(carplay_demo_touch(RTCP_TOUCH_DOWN,7,10,10)); /* stopped sink counters can be stale */
    puts("Actual demo worker: pairing/storage failure, replug, network/first-frame wait, quiescent close, pointer routing/overflow and manager exclusion passed");
}
