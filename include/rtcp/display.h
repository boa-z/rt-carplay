/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_DISPLAY_H
#define RTCP_DISPLAY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    uint16_t x, y, width, height;
} rtcp_display_rect_t;

typedef enum
{
    RTCP_APPEARANCE_AUTOMATIC = 0,
    RTCP_APPEARANCE_LIGHT = 1,
    RTCP_APPEARANCE_DARK = 2,
} rtcp_appearance_setting_t;

/* Optional Main Display profile. Initialize struct_size to sizeof(*profile).
 * Zero rectangles select the entire display/view respectively. Coordinates
 * refer to negotiated pixels, not a host framebuffer or video-layer viewport.
 * The host keeps the profile immutable for the duration of a build call.
 * This describes Type 110 only; it makes no altScreen/HEVC/Ultra claim. */
typedef struct
{
    size_t struct_size;
    const char *uuid; /* NULL keeps the Classic UUID; otherwise a UUID string. */
    rtcp_display_rect_t view_area;
    rtcp_display_rect_t safe_area;
    bool draw_outside_safe_area;
    bool ui_night;
    bool map_night;
    uint8_t appearance_setting; /* 0 automatic, 1 always light, 2 always dark */
} rtcp_display_profile_t;

bool rtcp_display_profile_validate(const rtcp_display_profile_t *, uint16_t width,
                                   uint16_t height);

/* Aspect-fit an encoded frame into a host viewport. Integer rounding centers
 * the result. Returns false for empty/overflowing geometry. No allocation. */
bool rtcp_display_fit(const rtcp_display_rect_t *viewport, uint16_t source_width,
                      uint16_t source_height, rtcp_display_rect_t *destination);
/* Map a panel point inside the actual aspect-fit destination to the negotiated
 * logical display. Bars/outside points are rejected. Output is unchanged on
 * failure; accepted coordinates are in [0, extent-1]. Ownership of a gesture
 * crossing the boundary remains the host's responsibility. */
bool rtcp_display_map_point(const rtcp_display_rect_t *destination,
                            uint16_t logical_width, uint16_t logical_height,
                            uint16_t panel_x, uint16_t panel_y,
                            uint16_t *logical_x, uint16_t *logical_y);

#ifdef __cplusplus
}
#endif
#endif
