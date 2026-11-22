/** @file device_time.h
 * Link-scoped iPhone clock updates. */
#ifndef RTCP_DEVICE_TIME_H
#define RTCP_DEVICE_TIME_H

#include <stdbool.h>
#include <stdint.h>

/** Presence flags distinguish absent values from zeros. Seconds use the UTC
 * Unix epoch. Both offsets are signed minutes, with DST separate from zone. */
typedef struct
{
    bool has_unix_seconds;
    int64_t unix_seconds;
    bool has_time_zone_offset;
    int16_t time_zone_offset_minutes;
    bool has_daylight_saving_offset;
    int8_t daylight_saving_offset_minutes;
} rtcp_device_time_t;

/** Borrow update on the iAP2 owner until return. A NULL update invalidates
 * the session's time data when explicitly enabled in the session config. */
typedef void (*rtcp_device_time_cb_t)(const rtcp_device_time_t *update, void *context);

#endif
