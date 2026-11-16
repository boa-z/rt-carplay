/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtcp/display.h"

static bool empty(const rtcp_display_rect_t *r)
{
    return r->x == 0U && r->y == 0U && r->width == 0U && r->height == 0U;
}

static bool valid(const rtcp_display_rect_t *r)
{
    return r != NULL && r->width != 0U && r->height != 0U &&
           (uint32_t)r->x + r->width <= UINT16_MAX &&
           (uint32_t)r->y + r->height <= UINT16_MAX;
}

static bool contains(const rtcp_display_rect_t *outer, const rtcp_display_rect_t *r)
{
    return valid(r) && r->x >= outer->x && r->y >= outer->y &&
           (uint32_t)r->x + r->width <= (uint32_t)outer->x + outer->width &&
           (uint32_t)r->y + r->height <= (uint32_t)outer->y + outer->height;
}

static bool uuid_valid(const char *s)
{
    for (unsigned i = 0U; i < 36U; ++i)
    {
        unsigned char c = (unsigned char)s[i];
        bool dash = i == 8U || i == 13U || i == 18U || i == 23U;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return s[36] == '\0';
}

bool rtcp_display_profile_validate(const rtcp_display_profile_t *p, uint16_t width,
                                   uint16_t height)
{
    if (width == 0U || height == 0U)
        return false;
    if (p == NULL)
        return true;
    if (p->struct_size != sizeof(*p) || p->appearance_setting > 2U ||
        (p->uuid != NULL && !uuid_valid(p->uuid)))
        return false;
    rtcp_display_rect_t whole = { 0U, 0U, width, height };
    const rtcp_display_rect_t *view = empty(&p->view_area) ? &whole : &p->view_area;
    return contains(&whole, view) &&
           (empty(&p->safe_area) || contains(view, &p->safe_area));
}

bool rtcp_display_fit(const rtcp_display_rect_t *viewport, uint16_t source_width,
                      uint16_t source_height, rtcp_display_rect_t *destination)
{
    if (!valid(viewport) || !source_width || !source_height || !destination)
        return false;
    rtcp_display_rect_t fit = *viewport;
    if ((uint32_t)viewport->width * source_height <=
        (uint32_t)viewport->height * source_width)
    {
        fit.height = (uint16_t)((uint32_t)viewport->width * source_height / source_width);
    }
    else
    {
        fit.width = (uint16_t)((uint32_t)viewport->height * source_width / source_height);
    }
    if (!fit.width || !fit.height)
        return false;
    fit.x += (uint16_t)((viewport->width - fit.width) / 2U);
    fit.y += (uint16_t)((viewport->height - fit.height) / 2U);
    *destination = fit;
    return true;
}

bool rtcp_display_map_point(const rtcp_display_rect_t *r,
                            uint16_t width, uint16_t height,
                            uint16_t x, uint16_t y, uint16_t *out_x, uint16_t *out_y)
{
    if (!valid(r) || !width || !height || !out_x || !out_y ||
        x < r->x || y < r->y || (uint32_t)x >= (uint32_t)r->x + r->width ||
        (uint32_t)y >= (uint32_t)r->y + r->height)
        return false;
    *out_x = (uint16_t)((uint32_t)(x - r->x) * width / r->width);
    *out_y = (uint16_t)((uint32_t)(y - r->y) * height / r->height);
    return true;
}
