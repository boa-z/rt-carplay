/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_NAVIGATION_H
#define RTCP_NAVIGATION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Classic iAP2 RouteGuidanceUpdate delta. NULL means absent; an empty
 * string clears the field. UTF-8 text is borrowed until callback return. */
typedef struct
{
    const char *road, *destination, *distance_remaining, *distance_to_maneuver;
    bool has_state;
    uint8_t state; /* Original iAP2 route state. */
} rtcp_route_guidance_t;

/* Classic iAP2 RouteGuidanceManeuverUpdate delta, keyed by index. */
typedef struct
{
    uint16_t index;
    const char *description, *road_after, *distance;
    bool has_type;
    uint8_t type; /* Original iAP2 maneuver type. */
} rtcp_route_maneuver_t;

typedef void (*rtcp_route_guidance_cb_t)(const rtcp_route_guidance_t *, void *context);
typedef void (*rtcp_route_maneuver_cb_t)(const rtcp_route_maneuver_t *, void *context);

#define RTCP_LOCATION_GGA 1U
#define RTCP_LOCATION_RMC 2U
#define RTCP_LOCATION_NMEA_MAX 82U

#ifdef __cplusplus
}
#endif
#endif
