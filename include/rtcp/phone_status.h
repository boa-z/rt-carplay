/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file phone_status.h
 * Accessory-side phone status notifications on one iAP2 runtime link. */
#ifndef RTCP_PHONE_STATUS_H
#define RTCP_PHONE_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** Which member of the status delta union is active. */
typedef enum
{
    RTCP_PHONE_STATUS_INVALIDATED,
    RTCP_PHONE_STATUS_POWER,
    RTCP_PHONE_STATUS_CELLULAR,
    RTCP_PHONE_STATUS_CALL,
} rtcp_phone_status_kind_t;

/** A delta from one runtime iAP2 link. Presence flags distinguish omitted
 * fields from zero values. Pointers borrow storage until callback return.
 * A NULL string is absent and an empty string clears it. Numeric states and
 * cellular arrays carry the original protocol values. An invalidation clears
 * all cached phone state for this link. */
typedef struct
{
    rtcp_phone_status_kind_t kind;
    union
    {
        struct
        {
            bool has_battery_level, has_charging_state, has_external_charger;
            uint16_t battery_level;
            uint8_t charging_state;
            bool external_charger;
        } power;
        struct
        {
            const uint8_t *signal_strength, *registration_status;
            size_t signal_strength_count, registration_status_count;
            const char *carrier;
            bool has_cellular_supported, cellular_supported;
            bool has_airplane_mode, airplane_mode;
        } cellular;
        struct
        {
            const char *remote_id, *display_name, *uuid;
            bool has_status, has_direction, has_disconnect_reason;
            uint8_t status, direction, disconnect_reason;
        } call;
    } data;
} rtcp_phone_status_t;

/** Deliver one borrowed status delta or invalidation on the link owner. */
typedef void (*rtcp_phone_status_cb_t)(const rtcp_phone_status_t *, void *context);

#ifdef __cplusplus
}
#endif
#endif
