/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "rtthread.h"
#include "../lubanlite_video/rtdevice.h"
#define RT_DEVICE_FLAG_INT_RX 0x100
#define RT_TOUCH_CTRL_DISABLE_INT 1
#define RT_TOUCH_CTRL_ENABLE_INT 2
#define RT_TOUCH_CTRL_GET_INFO 3
#define RT_TOUCH_EVENT_NONE 0
#define RT_TOUCH_EVENT_UP 1
#define RT_TOUCH_EVENT_DOWN 2
#define RT_TOUCH_EVENT_MOVE 3
struct rt_touch_info { unsigned point_num, range_x, range_y; };
struct rt_touch_data {
    uint8_t event, track_id;
    uint16_t x_coordinate, y_coordinate;
};
int rt_device_open(rt_device_t, unsigned);
int rt_device_close(rt_device_t);
int rt_device_set_rx_indicate(rt_device_t, rt_err_t (*)(rt_device_t, rt_size_t));
rt_size_t rt_device_read(rt_device_t, int, void *, rt_size_t);
