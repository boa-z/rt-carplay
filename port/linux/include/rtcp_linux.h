/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>

#include "rtcp/os.h"
#include "rtcp/net.h"
#include "rtcp/bt.h"
#include "rtcp/wifi.h"

/* POSIX implementations.  The BT and Wi-Fi instances are stubs that return
 * RTCP_ENOTSUP; a Linux simulator can subclass them for testing. */
extern const struct rtcp_os rtcp_os_linux;
extern const struct rtcp_net rtcp_net_linux;
extern const struct rtcp_bt rtcp_bt_linux;
extern const struct rtcp_wifi rtcp_wifi_linux;

/* Test/simulator helper: local port bound by a listener created with
 * rtcp_net_linux.listen(0).  Returns 0 on failure. */
uint16_t rtcp_net_linux_local_port(int fd);
