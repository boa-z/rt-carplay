/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtcp/display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    rtcp_display_profile_t p = { .struct_size = sizeof(p) };
    assert(rtcp_display_profile_validate(NULL, 1024, 600));
    assert(rtcp_display_profile_validate(&p, 1024, 600));
    p.uuid = "";
    assert(!rtcp_display_profile_validate(&p, 1024, 600));
    p.uuid = "01234567-89ab-cdef-0123-456789abcdef";
    p.view_area = (rtcp_display_rect_t){ 20, 10, 1000, 580 };
    p.safe_area = (rtcp_display_rect_t){ 30, 20, 980, 560 };
    assert(rtcp_display_profile_validate(&p, 1024, 600));
    p.safe_area.x = 0;
    assert(!rtcp_display_profile_validate(&p, 1024, 600));
    p.safe_area.x = 30;
    --p.struct_size;
    assert(!rtcp_display_profile_validate(&p, 1024, 600));
    rtcp_display_rect_t viewport = { 224, 0, 800, 600 }, dst;
    assert(rtcp_display_fit(&viewport, 1024, 600, &dst));
    assert(dst.x == 224 && dst.y == 66 && dst.width == 800 && dst.height == 468);
    uint16_t x = 99, y = 99;
    assert(!rtcp_display_map_point(&dst, 1024, 600, 500, 65, &x, &y));
    assert(x == 99 && y == 99);
    assert(rtcp_display_map_point(&dst, 1024, 600, 224, 66, &x, &y));
    assert(x == 0 && y == 0);
    assert(rtcp_display_map_point(&dst, 1024, 600, 1023, 533, &x, &y));
    assert(x < 1024 && y < 600);
    assert(!rtcp_display_map_point(&dst, 1024, 600, 1024, 534, &x, &y));
    /* Check every accepted panel point: no overflow or coordinates outside HID. */
    for (unsigned py = 0; py < 600; ++py)
        for (unsigned px = 0; px < 1024; ++px)
            if (rtcp_display_map_point(&dst, 1024, 600, (uint16_t)px, (uint16_t)py, &x, &y))
                assert(x < 1024 && y < 600);
    viewport.x = UINT16_MAX;
    assert(!rtcp_display_fit(&viewport, 1024, 600, &dst));
    viewport = (rtcp_display_rect_t){ 0, 0, 1, 600 };
    assert(!rtcp_display_fit(&viewport, UINT16_MAX, 1, &dst));
    puts("display profile and aspect-fit/HID bounds passed");
    return 0;
}
