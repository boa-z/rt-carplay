/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file navigation.h
 * Classic route-guidance deltas and accessory location selectors. */
#ifndef RTCP_NAVIGATION_H
#define RTCP_NAVIGATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** Classic iAP2 RouteGuidanceUpdate delta. NULL means absent; an empty
 * string clears the field. UTF-8 text is borrowed until callback return. */
typedef struct
{
    const char *road, *destination, *distance_remaining, *distance_to_maneuver;
    bool has_state;
    uint8_t state; /* Original iAP2 route state. */
} rtcp_route_guidance_t;

/** Classic iAP2 RouteGuidanceManeuverUpdate delta, keyed by index. */
typedef struct
{
    uint16_t index;
    const char *description, *road_after, *distance;
    bool has_type;
    uint8_t type; /* Original iAP2 maneuver type. */
} rtcp_route_maneuver_t;

/** Deliver a borrowed route summary on the iAP2 link owner. */
typedef void (*rtcp_route_guidance_cb_t)(const rtcp_route_guidance_t *, void *context);
/** Deliver a borrowed maneuver update on the iAP2 link owner. */
typedef void (*rtcp_route_maneuver_cb_t)(const rtcp_route_maneuver_t *, void *context);

/** An ordered maneuver list replaces the prior list when present. A present
 * empty list clears it. Maneuver descriptions arrive separately, keyed by
 * index through rtcp_route_maneuver_cb_t. Text and indices are borrowed
 * until callback return. Distances and units carry the original wire values. */
typedef struct
{
    rtcp_route_guidance_t guidance;
    bool invalidated;
    bool has_distance_remaining, has_distance_to_maneuver;
    uint32_t distance_remaining, distance_to_maneuver;
    bool has_remaining_units, has_maneuver_units, has_maneuver_count;
    uint8_t remaining_units, maneuver_units;
    uint16_t maneuver_count;
    const uint16_t *maneuver_indices;
    size_t maneuver_index_count;
    bool has_lane_index, has_lane_count, has_lanes_shown, lanes_shown;
    uint16_t lane_index, lane_count;
} rtcp_route_details_t;

/** Deliver a borrowed summary or invalidation. An invalidation clears the
 * host's route, maneuver and lane cache for this link. */
typedef void (*rtcp_route_details_cb_t)(const rtcp_route_details_t *, void *context);

#define RTCP_LANE_MAX_ANGLES 8U
/** Lane index and optional status, highlight and signed wire angles. */
typedef struct
{
    uint16_t index;
    bool has_status, has_highlight_angle;
    uint8_t status;
    int16_t highlight_angle;
    size_t angle_count;
    int16_t angles[RTCP_LANE_MAX_ANGLES];
} rtcp_lane_t;

/** One 0x5204 update, keyed by guidance index. Storage is borrowed until
 * return. Omitted description/lanes preserve the corresponding cached field. */
typedef struct
{
    uint16_t index;
    const char *description;
    const rtcp_lane_t *lanes;
    size_t lane_count;
} rtcp_lane_guidance_t;

/** Deliver borrowed lane data synchronously on the iAP2 link owner. */
typedef void (*rtcp_lane_guidance_cb_t)(const rtcp_lane_guidance_t *, void *context);

#define RTCP_LOCATION_GGA      1U
#define RTCP_LOCATION_RMC      2U
#define RTCP_LOCATION_PASCD    4U
#define RTCP_LOCATION_NMEA_MAX 82U

/** Transmission direction and stationary gear codes for PASCD. */
typedef enum
{
    RTCP_GEAR_PARK = 'P',
    RTCP_GEAR_REVERSE = 'R',
    RTCP_GEAR_NEUTRAL = 'N',
    RTCP_GEAR_DRIVE = 'D',
} rtcp_vehicle_gear_t;

#ifdef __cplusplus
}
#endif
#endif
