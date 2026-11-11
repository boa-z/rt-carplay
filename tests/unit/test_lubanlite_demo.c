/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Drive the actual automatic worker. UI compiles against SDK LVGL headers;
 * unused UI sections are removed, so this does not test framebuffer rendering. */
#include <assert.h>
#include <setjmp.h>
#include "../../examples/rtthread_app/demo_launcher_lubanlite.c"

static jmp_buf finished;
static unsigned mode, starts, pairs, closes, stops, net_checks, pictures, failed_waits;
static bool session_live, quiesced;
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
    assert(status.stage == DEMO_PAIR && starts == 1);
    ++pairs; session_live = true;
    return mode == 1 ? RTCP_EIO : RTCP_OK;
}
int carplay_usb_start_iap2(void) {
    assert(net_checks == 2 && status.stage == DEMO_CONNECT);
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
    if (++starts == 2) longjmp(finished, 1);
    return 0;
}
const char *carplay_app_pairing_code(void) { return "3939"; }
int carplay_app_wired_info(carplay_app_wired_info_t *info) {
    (void)info; ++net_checks;
    return net_checks == 1 ? RTCP_ENOENT : RTCP_OK;
}
bool carplay_app_is_running(void) { return true; }
int carplay_app_stop(void) {
    assert(quiesced && !session_live && closes >= (mode == 2 ? 2U : 1U));
    ++stops; return 0;
}
void carplay_video_sink_stats(unsigned *n, unsigned *f, unsigned *d, unsigned *e) {
    (void)n; (void)d; (void)e;
    *f = ++pictures == 1 ? 0 : 1;
}
int main(void) {
    for (mode = 0; mode < 3; ++mode) {
        starts = pairs = closes = stops = net_checks = pictures = failed_waits = 0;
        session_live = quiesced = false;
        memset(&status, 0, sizeof(status));
        if (setjmp(finished) == 0) demo_worker(NULL);
        assert(starts == 2 && pairs == 1 && stops == 1);
        assert(closes == (mode == 2 ? 2U : 1U));
        if (mode == 1) assert(failed_waits == 3 && net_checks == 0);
        else assert(net_checks == 2 && pictures == 2);
    }
    puts("Actual demo worker: pairing failure/replug, network readiness, first-frame wait and quiescent close passed");
}
