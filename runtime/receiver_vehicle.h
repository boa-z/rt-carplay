/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_RECEIVER_VEHICLE_INTERNAL_H
#define RTCP_RECEIVER_VEHICLE_INTERNAL_H
#include "rtcp/vehicle.h"
#define RTCP_RECEIVER_VEHICLE_MASK (RTCP_DATA_STREAM_MASK(RTCP_DATA_STREAM_VEHICLE) | \
                                    RTCP_DATA_STREAM_MASK(RTCP_DATA_STREAM_VEHICLE_HIGH))
typedef struct receiver_vehicle receiver_vehicle_t;
typedef int (*receiver_vehicle_send_t)(void *, rtcp_data_stream_kind_t, const uint8_t *, size_t);
receiver_vehicle_t *receiver_vehicle_create(rtcp_vehicle_t *, receiver_vehicle_send_t, void *);
void receiver_vehicle_destroy(receiver_vehicle_t *);
void receiver_vehicle_reset(receiver_vehicle_t *);
bool receiver_vehicle_body(receiver_vehicle_t *, rtcp_data_stream_kind_t, const uint8_t *, size_t);
void receiver_vehicle_closed(receiver_vehicle_t *, rtcp_data_stream_kind_t, int);
void receiver_vehicle_tick(receiver_vehicle_t *);
void receiver_vehicle_status(const receiver_vehicle_t *, rtcp_vehicle_delivery_status_t *);
#endif
