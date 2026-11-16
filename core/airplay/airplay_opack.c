/* SPDX-License-Identifier: GPL-3.0-or-later
 * OPACK tags and reference semantics cross-checked with LIVI opack.rs.
 * Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI).
 * See NOTICE for attribution. Bounded caller-owned C implementation.
 */
#include "rtcp/opack.h"
#include <limits.h>
#include <math.h>
#include <string.h>

typedef struct
{
    const uint8_t *input;
    size_t length, offset, capacity, used, seen_count;
    rtcp_opack_node_t *nodes;
    uint16_t seen[RTCP_OPACK_MAX_NODES];
    rtcp_opack_result_t error;
} reader_t;

static bool take(reader_t *r, size_t n, const uint8_t **bytes)
{
    if (n > r->length - r->offset)
    {
        r->error = RTCP_OPACK_INVALID;
        return false;
    }
    *bytes = r->input + r->offset;
    r->offset += n;
    return true;
}

static bool number(reader_t *r, unsigned width, uint64_t *out)
{
    const uint8_t *p;
    if (!take(r, width, &p))
        return false;
    *out = 0;
    for (unsigned i = 0; i < width; ++i) *out |= (uint64_t)p[i] << (8U * i);
    return true;
}

static bool utf8(const uint8_t *p, size_t length)
{
    for (size_t at = 0; at < length;)
    {
        uint32_t c = p[at++], minimum;
        unsigned extra;
        if (c < 0x80U)
            continue;
        if (c >= 0xc2U && c <= 0xdfU)
        {
            extra = 1;
            minimum = 0x80;
            c &= 0x1f;
        }
        else if (c >= 0xe0U && c <= 0xefU)
        {
            extra = 2;
            minimum = 0x800;
            c &= 0xf;
        }
        else if (c >= 0xf0U && c <= 0xf4U)
        {
            extra = 3;
            minimum = 0x10000;
            c &= 7;
        }
        else
            return false;
        if (extra > length - at)
            return false;
        for (unsigned i = 0; i < extra; ++i)
        {
            if ((p[at] & 0xc0U) != 0x80U)
                return false;
            c = (c << 6) | (p[at++] & 0x3fU);
        }
        if (c < minimum || c > 0x10ffffU || (c >= 0xd800U && c <= 0xdfffU))
            return false;
    }
    return true;
}

static bool read_value(reader_t *r, unsigned depth)
{
    if (depth > RTCP_OPACK_MAX_DEPTH || r->used >= r->capacity)
    {
        r->error = RTCP_OPACK_LIMIT;
        return false;
    }
    const uint8_t *tag_ptr;
    if (!take(r, 1, &tag_ptr))
        return false;
    uint8_t tag = *tag_ptr;
    size_t index = r->used++;
    rtcp_opack_node_t *node = &r->nodes[index];
    memset(node, 0, sizeof(*node));
    bool remember = false;
    uint64_t raw = 0;
    if (tag == 1 || tag == 2)
    {
        node->type = RTCP_OPACK_BOOL;
        node->value.boolean = tag == 1;
    }
    else if (tag == 4)
        node->type = RTCP_OPACK_NULL;
    else if (tag >= 8 && tag <= 0x2f)
    {
        node->type = RTCP_OPACK_INT;
        node->value.integer = tag - 8;
    }
    else if (tag >= 0x30 && tag <= 0x33)
    {
        unsigned width = 1U << (tag - 0x30);
        if (!number(r, width, &raw))
            return false;
        if (width < 8 && (raw & (UINT64_C(1) << (width * 8U - 1U))))
            raw |= UINT64_MAX << (width * 8U);
        node->type = RTCP_OPACK_INT;
        memcpy(&node->value.integer, &raw, sizeof(raw));
        remember = true;
    }
    else if (tag == 6 || tag == 0x35 || tag == 0x36)
    {
        unsigned width = tag == 0x35 ? 4 : 8;
        if (!number(r, width, &raw))
            return false;
        if (width == 4)
        {
            uint32_t bits = (uint32_t)raw;
            float f;
            memcpy(&f, &bits, sizeof(f));
            node->value.real = f;
        }
        else
            memcpy(&node->value.real, &raw, sizeof(raw));
        if (!isfinite(node->value.real))
        {
            r->error = RTCP_OPACK_INVALID;
            return false;
        }
        node->type = tag == 6 ? RTCP_OPACK_DATE : RTCP_OPACK_REAL;
        remember = true;
    }
    else if (tag == 5 || (tag >= 0x40 && tag <= 0x64) ||
             (tag >= 0x70 && tag <= 0x94))
    {
        node->type = tag == 5 ? RTCP_OPACK_UUID : tag < 0x70 ? RTCP_OPACK_STRING
                                                             : RTCP_OPACK_DATA;
        size_t length = tag == 5 ? 16U : (size_t)(tag - (tag < 0x70 ? 0x40 : 0x70));
        if (tag != 5 && length > 32)
        {
            if (!number(r, (unsigned)(length - 32U), &raw))
                return false;
            if (raw > RTCP_OPACK_MAX_BYTES)
            {
                r->error = RTCP_OPACK_LIMIT;
                return false;
            }
            length = (size_t)raw;
        }
        if (!take(r, length, &node->value.blob.bytes))
            return false;
        node->value.blob.length = length;
        if (node->type == RTCP_OPACK_STRING && !utf8(node->value.blob.bytes, length))
        {
            r->error = RTCP_OPACK_INVALID;
            return false;
        }
        remember = true;
    }
    else if (tag >= 0xa0 && tag <= 0xc4)
    {
        raw = tag - 0xa0U;
        if (tag > 0xc0 && !number(r, tag - 0xc0U, &raw))
            return false;
        if (raw >= r->seen_count)
        {
            r->error = RTCP_OPACK_INVALID;
            return false;
        }
        *node = r->nodes[r->seen[raw]];
    }
    else if (tag >= 0xd0 && tag <= 0xef)
    {
        node->type = tag < 0xe0 ? RTCP_OPACK_ARRAY : RTCP_OPACK_DICT;
        bool open = (tag & 0xfU) == 15U;
        unsigned count = tag & 0xfU;
        while (open || node->count < count)
        {
            if (open && r->offset < r->length && r->input[r->offset] == 3)
            {
                ++r->offset;
                break;
            }
            if (!read_value(r, depth + 1U))
                return false;
            if (node->type == RTCP_OPACK_DICT && !read_value(r, depth + 1U))
                return false;
            ++node->count;
        }
    }
    else
    {
        r->error = RTCP_OPACK_UNSUPPORTED;
        return false;
    }
    node->end = (uint16_t)r->used;
    if (remember)
        r->seen[r->seen_count++] = (uint16_t)index;
    return true;
}

rtcp_opack_result_t rtcp_opack_decode(const uint8_t *input, size_t length,
                                      rtcp_opack_node_t *nodes, size_t capacity,
                                      size_t *node_count)
{
    if (node_count)
        *node_count = 0;
    if (!input || !length || !nodes || !capacity || !node_count)
        return RTCP_OPACK_INVALID;
    if (length > RTCP_OPACK_MAX_BYTES || capacity > RTCP_OPACK_MAX_NODES)
        return RTCP_OPACK_LIMIT;
    reader_t r = { .input = input, .length = length, .capacity = capacity, .nodes = nodes, .error = RTCP_OPACK_INVALID };
    if (!read_value(&r, 0))
        return r.error;
    if (r.offset != length)
        return RTCP_OPACK_INVALID;
    *node_count = r.used;
    return RTCP_OPACK_OK;
}

typedef struct
{
    uint8_t *out;
    size_t capacity, used;
    rtcp_opack_result_t error;
} writer_t;
static bool put(writer_t *w, const void *bytes, size_t n)
{
    if (n > RTCP_OPACK_MAX_BYTES - w->used)
    {
        w->error = RTCP_OPACK_LIMIT;
        return false;
    }
    if (n > w->capacity - w->used)
    {
        w->error = RTCP_OPACK_OUTPUT_TOO_SMALL;
        return false;
    }
    if (n)
        memcpy(w->out + w->used, bytes, n);
    w->used += n;
    return true;
}
static bool byte(writer_t *w, uint8_t b)
{
    return put(w, &b, 1);
}
static bool little(writer_t *w, uint64_t value, unsigned width)
{
    for (unsigned i = 0; i < width; ++i)
        if (!byte(w, (uint8_t)(value >> (8U * i))))
            return false;
    return true;
}
static bool write_value(writer_t *w, const rtcp_opack_node_t *nodes, size_t count,
                        size_t at, unsigned depth)
{
    if (depth > RTCP_OPACK_MAX_DEPTH)
    {
        w->error = RTCP_OPACK_LIMIT;
        return false;
    }
    if (at >= count)
        return false;
    const rtcp_opack_node_t *n = &nodes[at];
    if (n->end <= at || n->end > count)
        return false;
    if (n->type == RTCP_OPACK_ARRAY || n->type == RTCP_OPACK_DICT)
    {
        bool open = n->count > 14;
        if (!byte(w, (uint8_t)((n->type == RTCP_OPACK_ARRAY ? 0xd0 : 0xe0) |
                               (open ? 15U : n->count))))
            return false;
        size_t child = at + 1U;
        unsigned total = (unsigned)n->count * (n->type == RTCP_OPACK_DICT ? 2U : 1U);
        for (unsigned i = 0; i < total; ++i)
        {
            if (child >= n->end || !write_value(w, nodes, count, child, depth + 1U))
                return false;
            child = nodes[child].end;
        }
        if (child != n->end)
            return false;
        return !open || byte(w, 3);
    }
    if (n->end != at + 1U || n->count != 0)
        return false;
    switch (n->type)
    {
    case RTCP_OPACK_NULL:
        return byte(w, 4);
    case RTCP_OPACK_BOOL:
        return byte(w, n->value.boolean ? 1 : 2);
    case RTCP_OPACK_INT:
    {
        int64_t v = n->value.integer;
        if (v >= 0 && v <= 39)
            return byte(w, (uint8_t)(v + 8));
        unsigned power = v >= INT8_MIN && v <= INT8_MAX ? 0 : v >= INT16_MIN && v <= INT16_MAX ? 1
                                                          : v >= INT32_MIN && v <= INT32_MAX   ? 2
                                                                                               : 3;
        return byte(w, (uint8_t)(0x30 + power)) && little(w, (uint64_t)v, 1U << power);
    }
    case RTCP_OPACK_REAL:
    case RTCP_OPACK_DATE:
    {
        if (!isfinite(n->value.real))
            return false;
        uint64_t bits;
        memcpy(&bits, &n->value.real, sizeof(bits));
        return byte(w, n->type == RTCP_OPACK_DATE ? 6 : 0x36) && little(w, bits, 8);
    }
    case RTCP_OPACK_STRING:
    case RTCP_OPACK_DATA:
    case RTCP_OPACK_UUID:
    {
        size_t len = n->value.blob.length;
        const uint8_t *p = n->value.blob.bytes;
        if ((len && !p) || len > RTCP_OPACK_MAX_BYTES)
            return false;
        if (n->type == RTCP_OPACK_UUID)
        {
            if (len != 16 || !byte(w, 5))
                return false;
        }
        else
        {
            if (n->type == RTCP_OPACK_STRING && !utf8(p, len))
                return false;
            uint8_t base = n->type == RTCP_OPACK_STRING ? 0x40 : 0x70;
            if (len <= 32)
            {
                if (!byte(w, (uint8_t)(base + len)))
                    return false;
            }
            else
            {
                unsigned width = len <= 0xff ? 1 : len <= 0xffff ? 2
                                                                 : 3;
                if (!byte(w, (uint8_t)(base + 32 + width)) || !little(w, len, width))
                    return false;
            }
        }
        return put(w, p, len);
    }
    default:
        return false;
    }
}

rtcp_opack_result_t rtcp_opack_encode(const rtcp_opack_node_t *nodes, size_t count,
                                      uint8_t *out, size_t capacity, size_t *length)
{
    if (length)
        *length = 0;
    if (!nodes || !count || !out || !length)
        return RTCP_OPACK_INVALID;
    if (count > RTCP_OPACK_MAX_NODES)
        return RTCP_OPACK_LIMIT;
    writer_t w = { .out = out, .capacity = capacity, .error = RTCP_OPACK_INVALID };
    if (!write_value(&w, nodes, count, 0, 0) || nodes[0].end != count)
        return w.error;
    *length = w.used;
    return RTCP_OPACK_OK;
}

size_t rtcp_opack_dict_get(const rtcp_opack_node_t *nodes, size_t count,
                           size_t dict, const char *key)
{
    if (!nodes || !key || dict >= count || nodes[dict].type != RTCP_OPACK_DICT ||
        nodes[dict].end > count)
        return SIZE_MAX;
    size_t at = dict + 1U, length = strlen(key), found = SIZE_MAX;
    for (unsigned i = 0; i < nodes[dict].count; ++i)
    {
        if (at >= nodes[dict].end || nodes[at].end <= at)
            return SIZE_MAX;
        const rtcp_opack_node_t *k = &nodes[at];
        at = k->end;
        if (at >= nodes[dict].end || nodes[at].end <= at)
            return SIZE_MAX;
        if (k->type == RTCP_OPACK_STRING && k->value.blob.length == length &&
            (length == 0 || (k->value.blob.bytes && memcmp(k->value.blob.bytes, key, length) == 0)))
        {
            if (found != SIZE_MAX)
                return SIZE_MAX; /* Ambiguous routing fields are invalid. */
            found = at;
        }
        at = nodes[at].end;
    }
    return at == nodes[dict].end ? found : SIZE_MAX;
}
