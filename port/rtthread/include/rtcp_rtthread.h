/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>

#include "rtcp/os.h"
#include "rtcp/net.h"
#include "rtcp/bt.h"

extern const struct rtcp_os rtcp_os_rtthread;
extern const struct rtcp_net rtcp_net_rtthread;

/* BT is a stub until the Zephyr Classic/RFCOMM integration is wired. */
extern const struct rtcp_bt rtcp_bt_rtthread;

/* Local port of a listener created with rtcp_net_rtthread.listen(0). */
uint16_t rtcp_net_rtthread_local_port(int fd);
