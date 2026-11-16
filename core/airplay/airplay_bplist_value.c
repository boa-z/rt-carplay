/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_bplist_value.h"
#include <string.h>

static bool valid(const rtcp_opack_node_t *n, size_t count, size_t at, unsigned depth)
{
    if (at >= count || depth > RTCP_OPACK_MAX_DEPTH || n[at].end <= at || n[at].end > count)
        return false;
    const rtcp_opack_node_t *v = &n[at];
    if (v->type == RTCP_OPACK_DICT || v->type == RTCP_OPACK_ARRAY)
    {
        size_t child = at + 1;
        for (unsigned i = 0; i < v->count; ++i)
        {
            if (v->type == RTCP_OPACK_DICT)
            {
                if (child >= v->end || n[child].type != RTCP_OPACK_STRING || !valid(n, count, child, depth + 1))
                    return false;
                child = n[child].end;
            }
            if (child >= v->end || !valid(n, count, child, depth + 1))
                return false;
            child = n[child].end;
        }
        return child == v->end;
    }
    if (v->end != at + 1 || v->count)
        return false;
    switch (v->type)
    {
    case RTCP_OPACK_NULL:
    case RTCP_OPACK_BOOL:
    case RTCP_OPACK_INT:
        return true;
    case RTCP_OPACK_STRING:
    case RTCP_OPACK_DATA:
        if (v->value.blob.length > AIRPLAY_BPLIST_VALUE_MAX_BYTES ||
            (!v->value.blob.bytes && v->value.blob.length))
            return false;
        if (v->type == RTCP_OPACK_STRING)
            for (size_t i = 0; i < v->value.blob.length; ++i)
                if (!v->value.blob.bytes[i] || v->value.blob.bytes[i] > 127)
                    return false;
        return true;
    default:
        return false;
    }
}
typedef struct
{
    uint8_t *bytes;
    size_t capacity, used;
    bool failed;
} writer_t;
static void byte(writer_t *w, uint8_t b)
{
    if (w->used >= w->capacity)
        w->failed = true;
    else
        w->bytes[w->used++] = b;
}
static void integer(writer_t *w, uint64_t n, unsigned width)
{
    for (unsigned i = width; i; --i) byte(w, (uint8_t)(n >> ((i - 1) * 8)));
}
static unsigned width(uint64_t n)
{
    return n <= UINT8_MAX ? 1 : n <= UINT16_MAX ? 2
                            : n <= UINT32_MAX   ? 4
                                                : 8;
}
static void number(writer_t *w, uint64_t n, bool negative)
{
    unsigned size = negative ? 8 : width(n);
    byte(w, (uint8_t)(0x10 | (size == 1 ? 0 : size == 2 ? 1
                                          : size == 4   ? 2
                                                        : 3)));
    integer(w, n, size);
}
static void count_header(writer_t *w, uint8_t kind, size_t count)
{
    byte(w, (uint8_t)(kind | (count < 15 ? count : 15)));
    if (count >= 15)
        number(w, count, false);
}
airplay_bplist_result_t airplay_bplist_encode_nodes(const rtcp_opack_node_t *n, size_t count,
                                                    uint8_t *out, size_t capacity, size_t *length)
{
    if (length)
        *length = 0;
    if (!n || !count || !out || !length)
        return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    if (count > AIRPLAY_BPLIST_VALUE_MAX_NODES)
        return AIRPLAY_BPLIST_TOO_LARGE;
    if (n[0].end != count || !valid(n, count, 0, 0))
        return AIRPLAY_BPLIST_INVALID_OBJECT;
    if (capacity > AIRPLAY_BPLIST_VALUE_MAX_BYTES)
        capacity = AIRPLAY_BPLIST_VALUE_MAX_BYTES;
    writer_t w = { .bytes = out, .capacity = capacity };
    uint32_t offsets[AIRPLAY_BPLIST_VALUE_MAX_NODES];
    static const uint8_t magic[] = "bplist00";
    for (unsigned i = 0; i < 8; ++i) byte(&w, magic[i]);
    for (size_t i = 0; i < count; ++i)
    {
        offsets[i] = (uint32_t)w.used;
        const rtcp_opack_node_t *v = &n[i];
        switch (v->type)
        {
        case RTCP_OPACK_NULL:
            byte(&w, 0);
            break;
        case RTCP_OPACK_BOOL:
            byte(&w, v->value.boolean ? 9 : 8);
            break;
        case RTCP_OPACK_INT:
            number(&w, (uint64_t)v->value.integer, v->value.integer < 0);
            break;
        case RTCP_OPACK_STRING:
        case RTCP_OPACK_DATA:
            count_header(&w, v->type == RTCP_OPACK_STRING ? 0x50 : 0x40, v->value.blob.length);
            for (size_t j = 0; j < v->value.blob.length; ++j) byte(&w, v->value.blob.bytes[j]);
            break;
        case RTCP_OPACK_ARRAY:
        case RTCP_OPACK_DICT:
        {
            count_header(&w, v->type == RTCP_OPACK_DICT ? 0xd0 : 0xa0, v->count);
            size_t child = i + 1;
            for (unsigned j = 0; j < v->count; ++j)
            {
                integer(&w, child, 1);
                child = n[child].end;
                if (v->type == RTCP_OPACK_DICT)
                    child = n[child].end;
            }
            if (v->type == RTCP_OPACK_DICT)
            {
                child = i + 1;
                for (unsigned j = 0; j < v->count; ++j)
                {
                    child = n[child].end;
                    integer(&w, child, 1);
                    child = n[child].end;
                }
            }
            break;
        }
        default:
            return AIRPLAY_BPLIST_INVALID_OBJECT;
        }
    }
    size_t table = w.used;
    unsigned offset_width = width(table);
    for (size_t i = 0; i < count; ++i) integer(&w, offsets[i], offset_width);
    for (unsigned i = 0; i < 6; ++i) byte(&w, 0);
    byte(&w, (uint8_t)offset_width);
    byte(&w, 1);
    integer(&w, count, 8);
    integer(&w, 0, 8);
    integer(&w, table, 8);
    if (w.failed)
        return AIRPLAY_BPLIST_OUTPUT_TOO_SMALL;
    *length = w.used;
    return AIRPLAY_BPLIST_OK;
}
