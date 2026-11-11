/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Luban-Lite application entry for rt-carplay.
 *
 * Copy this directory to application/rt-thread/rtcarplay/ and select the
 * matching _rtcarplay_defconfig.  Without RTCARPLAY_AUTO_START the receiver
 * waits for the carplay_start MSH command, which is the recommended order
 * for first bring-up (the console then shows the pairing PIN).
 */
#include <rtthread.h>

#include "carplay_app.h"

#ifdef RTCARPLAY_AUTO_START
static void rtcarplay_autostart(void)
{
    carplay_app_config_t config;
    carplay_app_config_defaults(&config);
    config.width_pixels = RTCARPLAY_SCREEN_WIDTH;
    config.height_pixels = RTCARPLAY_SCREEN_HEIGHT;
    config.rotation_degrees = RTCARPLAY_ROTATION;

    int result = carplay_app_start(&config);
    if (result != RTCP_OK) {
        rt_kprintf("rt-carplay autostart failed: %d\n", result);
    }
}
#endif

int main(void)
{
#if defined(RTCARPLAY_DEMO_LAUNCHER)
    rt_kprintf("rt-carplay: automatic wired demo; connect an unlocked iPhone\n");
#elif defined(RTCARPLAY_AUTO_START)
    rtcarplay_autostart();
#else
    rt_kprintf("rt-carplay: run carplay_start, then carplay_status "
               "(PIN is printed at start)\n");
#endif
    return 0;
}
