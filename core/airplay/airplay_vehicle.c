/* SPDX-License-Identifier: GPL-3.0-or-later
 * Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI).
 * Command/envelope/catalogue shapes cross-checked with vehicle_state.rs and
 * vehicle_state/wire.rs at 64a05b68358db52ed887169143afd5944fd6f1f7.
 * See NOTICE. No Apple assets or vehicle-specific catalogue is included.
 */
#include "rtcp/vehicle.h"
#include "rtcp/opack.h"
#include "rtcp/rtcp.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX RTCP_VEHICLE_MAX_CHARACTERISTICS
#define NO_VALUE INT64_C(-6727)
#define NO_COMMAND INT64_C(-6735)
#define WIRE_BYTES 16384U

typedef struct {
    rtcp_vehicle_characteristic_t spec;
    rtcp_vehicle_value_t value;
    bool has_value, subscribed, dirty;
} entry_t;
typedef struct {
    rtcp_opack_node_t nodes[RTCP_OPACK_MAX_NODES];
    size_t used;
    bool failed;
} builder_t;
typedef struct {
    rtcp_vehicle_control_t spec;
    rtcp_vehicle_control_parameter_t request[RTCP_VEHICLE_MAX_CONTROL_PARAMETERS];
    rtcp_vehicle_control_parameter_t response[RTCP_VEHICLE_MAX_CONTROL_PARAMETERS];
    char names[2][RTCP_VEHICLE_MAX_CONTROL_PARAMETERS][RTCP_VEHICLE_MAX_PARAMETER_NAME+1];
} control_t;
typedef struct {
    size_t count;
    control_t entries[RTCP_VEHICLE_MAX_CONTROLS];
    rtcp_vehicle_control_values_t request[RTCP_VEHICLE_MAX_CONTROLS];
    rtcp_vehicle_control_values_t response[RTCP_VEHICLE_MAX_CONTROLS];
    bool (*prepare)(void *, int64_t, const rtcp_vehicle_control_values_t *, rtcp_vehicle_control_values_t *);
    void (*commit)(void *, int64_t, const rtcp_vehicle_control_values_t *);
} controls_t;
struct rtcp_vehicle {
    entry_t entries[MAX];
    size_t count;
    char plugin_name[33];
    int64_t plugin_id;
    bool (*validate_write)(void *, int64_t, const rtcp_vehicle_value_t *);
    void (*did_write)(void *, int64_t, const rtcp_vehicle_value_t *);
    void *context;
    rtcp_vehicle_status_t stats;
    rtcp_opack_node_t request[RTCP_OPACK_MAX_NODES];
    builder_t builder;
    uint8_t inner[WIRE_BYTES];
    entry_t staged[MAX];
    controls_t *controls;
};

static size_t add(builder_t *b, rtcp_opack_type_t type)
{
    if (b->used == RTCP_OPACK_MAX_NODES) { b->failed = true; return 0; }
    size_t at = b->used++;
    b->nodes[at] = (rtcp_opack_node_t){.type = type, .end = (uint16_t)b->used};
    return at;
}
static void text(builder_t *b, const char *s)
{
    size_t at = add(b, RTCP_OPACK_STRING);
    if (!b->failed) {
        b->nodes[at].value.blob.bytes = (const uint8_t *)s;
        b->nodes[at].value.blob.length = strlen(s);
    }
}
static void integer(builder_t *b, int64_t n)
{
    size_t at = add(b, RTCP_OPACK_INT);
    if (!b->failed) b->nodes[at].value.integer = n;
}
static void boolean(builder_t *b, bool value)
{
    size_t at = add(b, RTCP_OPACK_BOOL);
    if (!b->failed) b->nodes[at].value.boolean = value;
}
static void finish(builder_t *b, size_t at, size_t count)
{
    if (!b->failed) {
        b->nodes[at].end = (uint16_t)b->used;
        b->nodes[at].count = (uint16_t)count;
    }
}
static void value_node(builder_t *b, const rtcp_vehicle_value_t *v)
{
    size_t at;
    switch (v->format) {
    case RTCP_VEHICLE_BOOL: boolean(b, v->as.boolean); break;
    case RTCP_VEHICLE_FLOAT:
        at = add(b, RTCP_OPACK_REAL);
        if (!b->failed) b->nodes[at].value.real = v->as.real;
        break;
    case RTCP_VEHICLE_STRING: case RTCP_VEHICLE_DATA:
        at = add(b, v->format == RTCP_VEHICLE_STRING ? RTCP_OPACK_STRING : RTCP_OPACK_DATA);
        if (!b->failed) {
            b->nodes[at].value.blob.bytes = v->as.blob.bytes;
            b->nodes[at].value.blob.length = v->as.blob.length;
        }
        break;
    default: integer(b, v->as.integer); break;
    }
}
static void number_node(builder_t *b, double n, rtcp_vehicle_format_t format)
{
    rtcp_vehicle_value_t v = {.format = format};
    if (format == RTCP_VEHICLE_FLOAT) v.as.real = n;
    else v.as.integer = (int64_t)n;
    value_node(b, &v);
}
static bool same_value(const rtcp_vehicle_value_t *a, const rtcp_vehicle_value_t *b)
{
    if (a->format != b->format) return false;
    switch (a->format) {
    case RTCP_VEHICLE_BOOL: return a->as.boolean == b->as.boolean;
    case RTCP_VEHICLE_FLOAT: return a->as.real == b->as.real;
    case RTCP_VEHICLE_STRING: case RTCP_VEHICLE_DATA:
        return a->as.blob.length == b->as.blob.length &&
            memcmp(a->as.blob.bytes, b->as.blob.bytes, a->as.blob.length) == 0;
    default: return a->as.integer == b->as.integer;
    }
}
static bool valid_value(const rtcp_vehicle_characteristic_t *s, const rtcp_vehicle_value_t *v)
{
    if (s->format != v->format) return false;
    switch (v->format) {
    case RTCP_VEHICLE_BOOL: return true;
    case RTCP_VEHICLE_STRING: case RTCP_VEHICLE_DATA: {
        if (v->as.blob.length > s->maximum_length) return false;
        if (v->format == RTCP_VEHICLE_DATA) return true;
        /* Reuse the codec's strict UTF-8 validator, including embedded NUL. */
        rtcp_opack_node_t node = {.type = RTCP_OPACK_STRING, .end = 1};
        node.value.blob.bytes = v->as.blob.bytes;
        node.value.blob.length = v->as.blob.length;
        uint8_t encoded[RTCP_VEHICLE_MAX_VALUE_BYTES + 5]; size_t length;
        return rtcp_opack_encode(&node, 1, encoded, sizeof(encoded), &length) == RTCP_OPACK_OK;
    }
    case RTCP_VEHICLE_UINT8:
        if (v->as.integer < 0 || v->as.integer > UINT8_MAX) return false;
        break;
    case RTCP_VEHICLE_UINT32:
        if (v->as.integer < 0 || v->as.integer > UINT32_MAX) return false;
        break;
    case RTCP_VEHICLE_UINT64:
        if (v->as.integer < 0) return false;
        break;
    case RTCP_VEHICLE_INT32:
        if (v->as.integer < INT32_MIN || v->as.integer > INT32_MAX) return false;
        break;
    case RTCP_VEHICLE_FLOAT:
        if (!isfinite(v->as.real)) return false;
        break;
    default: return false;
    }
    if (s->has_range) {
        if (s->format != RTCP_VEHICLE_FLOAT) {
            /* valid_spec bounds integral metadata before casting. UINT64 has
             * nonnegative minimum; INT32 differences fit int64. Exact modulo
             * also preserves low bits above double's 53-bit integer range. */
            int64_t minimum = (int64_t)s->minimum, maximum = (int64_t)s->maximum;
            return v->as.integer >= minimum && v->as.integer <= maximum &&
                (v->as.integer - minimum) % (int64_t)s->step == 0;
        }
        double n = v->as.real;
        if (n < s->minimum || n > s->maximum) return false;
        double step = (n - s->minimum) / s->step;
        /* Tolerance only for binary rounding; do not clamp phone writes. */
        if (!isfinite(step) || fabs(step - round(step)) > 1e-7) return false;
    }
    return true;
}
static bool valid_spec(const rtcp_vehicle_characteristic_t *s)
{
    if (s->iid <= 0 || s->accessory_iid <= 0 || s->service_iid <= 0 ||
        s->iid == s->service_iid || s->iid == s->accessory_iid ||
        s->service_iid == s->accessory_iid || s->type < 0 ||
        s->accessory_type < 0 || s->service_type < 0 ||
        s->owner < RTCP_VEHICLE_PHONE || s->owner > RTCP_VEHICLE_SHARED) return false;
    switch (s->format) {
    case RTCP_VEHICLE_BOOL: case RTCP_VEHICLE_UINT8: case RTCP_VEHICLE_UINT32:
    case RTCP_VEHICLE_UINT64: case RTCP_VEHICLE_INT32: case RTCP_VEHICLE_FLOAT: break;
    case RTCP_VEHICLE_STRING: case RTCP_VEHICLE_DATA:
        if (!s->maximum_length || s->maximum_length > RTCP_VEHICLE_MAX_VALUE_BYTES) return false;
        break;
    default: return false;
    }
    if (s->has_range) {
        if (s->format == RTCP_VEHICLE_BOOL || s->format == RTCP_VEHICLE_STRING ||
            s->format == RTCP_VEHICLE_DATA || !isfinite(s->minimum) ||
            !isfinite(s->maximum) || !isfinite(s->step) || s->step <= 0 ||
            s->minimum > s->maximum) return false;
        /* Integral metadata is encoded as int64, without an unsafe cast at
         * the rounded double representation of INT64_MAX. */
        if (s->format != RTCP_VEHICLE_FLOAT &&
            (s->minimum < (double)INT64_MIN || s->maximum >= 0x1p63 ||
             s->step >= 0x1p63 || trunc(s->minimum) != s->minimum ||
             trunc(s->maximum) != s->maximum || trunc(s->step) != s->step)) return false;
        if ((s->format == RTCP_VEHICLE_UINT8 && (s->minimum < 0 || s->maximum > UINT8_MAX)) ||
            (s->format == RTCP_VEHICLE_UINT32 && (s->minimum < 0 || s->maximum > UINT32_MAX)) ||
            (s->format == RTCP_VEHICLE_UINT64 && s->minimum < 0) ||
            (s->format == RTCP_VEHICLE_INT32 &&
             (s->minimum < INT32_MIN || s->maximum > INT32_MAX))) return false;
    }
    return !s->has_initial || valid_value(s, &s->initial);
}
static size_t find(const rtcp_vehicle_t *v, int64_t iid)
{
    for (size_t i = 0; i < v->count; ++i) if (v->entries[i].spec.iid == iid) return i;
    return SIZE_MAX;
}
static rtcp_vehicle_characteristic_t parameter_spec(const rtcp_vehicle_control_parameter_t *p)
{
    return (rtcp_vehicle_characteristic_t){.accessory_iid=1,.service_iid=2,.iid=3,
        .format=p->format,.maximum_length=p->maximum_length,.owner=RTCP_VEHICLE_CAR};
}
static bool valid_parameters(const rtcp_vehicle_control_parameter_t *p, size_t count)
{
    if (count>RTCP_VEHICLE_MAX_CONTROL_PARAMETERS || (count && !p)) return false;
    for (size_t i=0;i<count;++i) {
        if (!p[i].name || !p[i].name[0] || strlen(p[i].name)>RTCP_VEHICLE_MAX_PARAMETER_NAME) return false;
        for (const unsigned char *c=(const unsigned char *)p[i].name;*c;++c)
            if (*c<0x20 || *c>0x7e) return false;
        rtcp_vehicle_characteristic_t spec=parameter_spec(&p[i]);
        if (!valid_spec(&spec)) return false;
        for (size_t j=0;j<i;++j) if (!strcmp(p[i].name,p[j].name)) return false;
    }
    return true;
}
static bool valid_controls(const rtcp_vehicle_config_t *c)
{
    if (c->control_count>RTCP_VEHICLE_MAX_CONTROLS ||
        (c->control_count && (!c->controls || !c->prepare_control))) return false;
    for (size_t i=0;i<c->control_count;++i) {
        const rtcp_vehicle_control_t *s=&c->controls[i]; bool service=false;
        if (s->iid<=0 || s->type<0 || !valid_parameters(s->request,s->request_count) ||
            !valid_parameters(s->response,s->response_count)) return false;
        for (size_t j=0;j<c->count;++j) {
            const rtcp_vehicle_characteristic_t *p=&c->characteristics[j];
            if (s->iid==p->iid || s->iid==p->service_iid || s->iid==p->accessory_iid) return false;
            if (s->service_iid==p->service_iid) service=true;
        }
        if (!service) return false;
        for (size_t j=0;j<i;++j) if (s->iid==c->controls[j].iid) return false;
    }
    return true;
}
static void copy_parameters(rtcp_vehicle_control_parameter_t *out, char names[][RTCP_VEHICLE_MAX_PARAMETER_NAME+1],
    const rtcp_vehicle_control_parameter_t *in, size_t count)
{
    for (size_t i=0;i<count;++i) {
        strcpy(names[i],in[i].name); out[i]=in[i]; out[i].name=names[i];
    }
}
static size_t find_control(const rtcp_vehicle_t *v, int64_t iid)
{
    if (v->controls) for (size_t i=0;i<v->controls->count;++i)
        if (v->controls->entries[i].spec.iid==iid) return i;
    return SIZE_MAX;
}
int rtcp_vehicle_create(const rtcp_vehicle_config_t *config, rtcp_vehicle_t **out)
{
    if (!out) return RTCP_EINVAL;
    *out = NULL;
    if (!config || config->struct_size != sizeof(*config) || !config->plugin_name ||
        !config->plugin_name[0] || strlen(config->plugin_name) > 32 ||
        config->plugin_id < 0 || !config->characteristics ||
        !config->count || config->count > MAX) return RTCP_EINVAL;
    for (size_t i = 0; i < config->count; ++i) {
        const rtcp_vehicle_characteristic_t *s = &config->characteristics[i];
        if (!valid_spec(s)) return RTCP_EINVAL;
        for (size_t j = 0; j < i; ++j) {
            const rtcp_vehicle_characteristic_t *p = &config->characteristics[j];
            if (p->iid == s->iid || p->iid == s->accessory_iid || p->iid == s->service_iid ||
                s->iid == p->accessory_iid || s->iid == p->service_iid ||
                p->service_iid == s->accessory_iid || s->service_iid == p->accessory_iid ||
                (p->accessory_iid == s->accessory_iid && p->accessory_type != s->accessory_type) ||
                (p->service_iid == s->service_iid &&
                 (p->accessory_iid != s->accessory_iid || p->service_type != s->service_type)))
                return RTCP_EINVAL;
        }
    }
    if (!valid_controls(config)) return RTCP_EINVAL;
    rtcp_vehicle_t *v = calloc(1, sizeof(*v));
    if (!v) return RTCP_ENOMEM;
    v->count = config->count; v->plugin_id = config->plugin_id;
    memcpy(v->plugin_name, config->plugin_name, strlen(config->plugin_name) + 1);
    v->validate_write = config->validate_write; v->did_write = config->did_write;
    v->context = config->context;
    for (size_t i = 0; i < v->count; ++i) {
        v->entries[i].spec = config->characteristics[i];
        v->entries[i].has_value = v->entries[i].spec.has_initial;
        if (v->entries[i].has_value) v->entries[i].value = v->entries[i].spec.initial;
    }
    if (config->control_count) {
        v->controls=calloc(1,sizeof(*v->controls));
        if (!v->controls) { free(v); return RTCP_ENOMEM; }
        v->controls->count=config->control_count;
        v->controls->prepare=config->prepare_control; v->controls->commit=config->did_control;
        for (size_t i=0;i<config->control_count;++i) {
            control_t *c=&v->controls->entries[i]; c->spec=config->controls[i];
            copy_parameters(c->request,c->names[0],c->spec.request,c->spec.request_count);
            copy_parameters(c->response,c->names[1],c->spec.response,c->spec.response_count);
            c->spec.request=c->request; c->spec.response=c->response;
        }
    }
    *out = v;
    return RTCP_OK;
}
void rtcp_vehicle_destroy(rtcp_vehicle_t *v) { if (v) free(v->controls); free(v); }
void rtcp_vehicle_reset_session(rtcp_vehicle_t *v)
{
    if (v) for (size_t i = 0; i < v->count; ++i)
        v->entries[i].subscribed = v->entries[i].dirty = false;
}
int rtcp_vehicle_status(const rtcp_vehicle_t *v, rtcp_vehicle_status_t *out)
{
    if (!v || !out) return RTCP_EINVAL;
    *out = v->stats; out->characteristics = v->count;
    out->controls=v->controls ? v->controls->count : 0;
    for (size_t i = 0; i < v->count; ++i) {
        out->subscribed += v->entries[i].subscribed;
        out->pending_notifications += v->entries[i].dirty;
    }
    return RTCP_OK;
}
int rtcp_vehicle_get(const rtcp_vehicle_t *v, int64_t iid,
                      rtcp_vehicle_value_t *out, bool *has_value)
{
    if (!v || !out || !has_value) return RTCP_EINVAL;
    memset(out, 0, sizeof(*out)); *has_value = false;
    size_t i = find(v, iid);
    if (i == SIZE_MAX) return RTCP_ENOENT;
    *has_value = v->entries[i].has_value;
    if (*has_value) *out = v->entries[i].value;
    return RTCP_OK;
}
static void set(entry_t *e, const rtcp_vehicle_value_t *value)
{
    if (!e->has_value || !same_value(&e->value, value)) {
        e->value = *value; e->has_value = true;
        if (e->subscribed) e->dirty = true;
    }
}
int rtcp_vehicle_set(rtcp_vehicle_t *v, int64_t iid, const rtcp_vehicle_value_t *value)
{
    if (!v || !value) return RTCP_EINVAL;
    size_t i = find(v, iid);
    if (i == SIZE_MAX) return RTCP_ENOENT;
    entry_t *e = &v->entries[i];
    if (e->spec.owner == RTCP_VEHICLE_PHONE || !e->spec.mutable_value) return RTCP_ESTATE;
    if (!valid_value(&e->spec, value)) return RTCP_EINVAL;
    set(e, value); return RTCP_OK;
}

static void describe(builder_t *b, const entry_t *e)
{
    const rtcp_vehicle_characteristic_t *s = &e->spec;
    size_t at = add(b, RTCP_OPACK_DICT), pairs = 7;
    text(b, "iid"); integer(b, s->iid);
    text(b, "type"); integer(b, s->type);
    text(b, "format"); integer(b, s->format);
    text(b, "mutable"); boolean(b, s->mutable_value);
    text(b, "writable"); boolean(b, s->mutable_value && s->owner != RTCP_VEHICLE_CAR);
    text(b, "oemWritable"); boolean(b, s->mutable_value && s->owner != RTCP_VEHICLE_PHONE);
    text(b, "priority"); integer(b, 1);
    if (s->has_range) {
        text(b, "minimumValue"); number_node(b, s->minimum, s->format);
        text(b, "maximumValue"); number_node(b, s->maximum, s->format);
        text(b, "stepValue"); number_node(b, s->step, s->format); pairs += 3;
    }
    if (s->has_units) { text(b, "units"); integer(b, s->units); ++pairs; }
    if (s->format == RTCP_VEHICLE_STRING || s->format == RTCP_VEHICLE_DATA) {
        text(b, "maximumLength"); integer(b, (int64_t)s->maximum_length); ++pairs;
    }
    if (e->has_value) { text(b, "initialValue"); value_node(b, &e->value); ++pairs; }
    finish(b, at, pairs);
}
static void describe_parameters(builder_t *b, const rtcp_vehicle_control_parameter_t *params, size_t count)
{
    size_t arr=add(b,RTCP_OPACK_ARRAY);
    for (size_t i=0;i<count;++i) {
        size_t dict=add(b,RTCP_OPACK_DICT);
        text(b,"name"); text(b,params[i].name); text(b,"format"); integer(b,params[i].format);
        finish(b,dict,2);
    }
    finish(b,arr,count);
}
static void describe_controls(const rtcp_vehicle_t *v, builder_t *b, int64_t service)
{
    size_t arr=add(b,RTCP_OPACK_ARRAY), count=0;
    if (v->controls) for (size_t i=0;i<v->controls->count;++i) {
        const rtcp_vehicle_control_t *c=&v->controls->entries[i].spec;
        if (c->service_iid!=service) continue;
        ++count; size_t dict=add(b,RTCP_OPACK_DICT), pairs=4;
        text(b,"iid"); integer(b,c->iid); text(b,"type"); integer(b,c->type);
        text(b,"sender"); integer(b,0); text(b,"hasResponse"); boolean(b,true);
        if (c->urgent) { text(b,"priority"); integer(b,1); ++pairs; }
        if (c->request_count) { text(b,"requestParameters"); describe_parameters(b,c->request,c->request_count); ++pairs; }
        if (c->response_count) { text(b,"responseParameters"); describe_parameters(b,c->response,c->response_count); ++pairs; }
        finish(b,dict,pairs);
    }
    finish(b,arr,count);
}
static void accessories(rtcp_vehicle_t *v, builder_t *b)
{
    size_t arr = add(b, RTCP_OPACK_ARRAY), count = 0;
    for (size_t i = 0; i < v->count; ++i) {
        const rtcp_vehicle_characteristic_t *s = &v->entries[i].spec;
        bool seen = false;
        for (size_t j = 0; j < i; ++j)
            if (v->entries[j].spec.accessory_iid == s->accessory_iid) seen = true;
        if (seen) continue;
        ++count;
        size_t acc = add(b, RTCP_OPACK_DICT);
        text(b, "iid"); integer(b, s->accessory_iid);
        text(b, "type"); integer(b, s->accessory_type);
        text(b, "version"); text(b, "1.0");
        text(b, "services"); size_t services = add(b, RTCP_OPACK_ARRAY), nservices = 0;
        for (size_t j = 0; j < v->count; ++j) {
            const rtcp_vehicle_characteristic_t *t = &v->entries[j].spec;
            if (t->accessory_iid != s->accessory_iid) continue;
            seen = false;
            for (size_t k = 0; k < j; ++k)
                if (v->entries[k].spec.service_iid == t->service_iid) seen = true;
            if (seen) continue;
            ++nservices;
            size_t svc = add(b, RTCP_OPACK_DICT);
            text(b, "iid"); integer(b, t->service_iid);
            text(b, "type"); integer(b, t->service_type);
            text(b, "characteristics"); size_t chars = add(b, RTCP_OPACK_ARRAY), nchars = 0;
            for (size_t k = 0; k < v->count; ++k)
                if (v->entries[k].spec.service_iid == t->service_iid) {
                    describe(b, &v->entries[k]); ++nchars;
                }
            finish(b, chars, nchars);
            bool controls=false;
            if (v->controls) for (size_t k=0;k<v->controls->count;++k)
                if (v->controls->entries[k].spec.service_iid==t->service_iid) controls=true;
            if (controls) { text(b,"controls"); describe_controls(v,b,t->service_iid); }
            finish(b, svc, controls ? 4 : 3);
        }
        finish(b, services, nservices); finish(b, acc, 4);
    }
    finish(b, arr, count);
}
static int encode(builder_t *b, uint8_t *out, size_t capacity, size_t *length)
{
    *length = 0;
    if (b->failed) return RTCP_ENOMEM;
    rtcp_opack_result_t rc = rtcp_opack_encode(b->nodes, b->used, out, capacity, length);
    return rc == RTCP_OPACK_OK ? RTCP_OK :
        rc == RTCP_OPACK_LIMIT || rc == RTCP_OPACK_OUTPUT_TOO_SMALL ? RTCP_ENOMEM : RTCP_EINVAL;
}
int rtcp_vehicle_info(rtcp_vehicle_t *v, uint8_t *out, size_t capacity, size_t *length)
{
    if (length) *length = 0;
    if (!v || !out || !length) return RTCP_EINVAL;
    builder_t *b = &v->builder; b->used = 0; b->failed = false;
    size_t root = add(b, RTCP_OPACK_DICT);
    text(b, "protocolVersion"); text(b, "1.0");
    text(b, "pluginCount"); integer(b, 1);
    text(b, "pluginMapping"); size_t mapping = add(b, RTCP_OPACK_DICT);
    text(b, v->plugin_name); integer(b, v->plugin_id); finish(b, mapping, 1);
    text(b, "pluginConfigs"); size_t plugins = add(b, RTCP_OPACK_ARRAY);
    size_t plugin = add(b, RTCP_OPACK_DICT);
    text(b, "name"); text(b, v->plugin_name);
    text(b, "pluginID"); integer(b, v->plugin_id);
    text(b, "accessories"); accessories(v, b);
    finish(b, plugin, 3); finish(b, plugins, 1); finish(b, root, 4);
    return encode(b, out, capacity, length);
}
static int wrap(rtcp_vehicle_t *v, uint8_t *out, size_t capacity, size_t *length)
{
    size_t inner_length;
    int rc = encode(&v->builder, v->inner, sizeof(v->inner), &inner_length);
    if (rc != RTCP_OK) return rc;
    builder_t *b = &v->builder; b->used = 0; b->failed = false;
    size_t root = add(b, RTCP_OPACK_DICT);
    text(b, "pluginID"); integer(b, v->plugin_id);
    text(b, "pluginData"); size_t at = add(b, RTCP_OPACK_DATA);
    b->nodes[at].value.blob.bytes = v->inner;
    b->nodes[at].value.blob.length = inner_length;
    finish(b, root, 2);
    return encode(b, out, capacity, length);
}
static bool named(const rtcp_opack_node_t *n, const char *s)
{
    return n->type == RTCP_OPACK_STRING && n->value.blob.length == strlen(s) &&
        memcmp(n->value.blob.bytes, s, strlen(s)) == 0;
}
static bool from_node(const rtcp_vehicle_characteristic_t *s,
                      const rtcp_opack_node_t *n, rtcp_vehicle_value_t *out)
{
    memset(out, 0, sizeof(*out)); out->format = s->format;
    if (s->format == RTCP_VEHICLE_BOOL) {
        if (n->type != RTCP_OPACK_BOOL) return false;
        out->as.boolean = n->value.boolean;
    } else if (s->format == RTCP_VEHICLE_FLOAT) {
        if (n->type != RTCP_OPACK_REAL && n->type != RTCP_OPACK_INT) return false;
        out->as.real = n->type == RTCP_OPACK_REAL ? n->value.real : (double)n->value.integer;
    } else if (s->format == RTCP_VEHICLE_STRING || s->format == RTCP_VEHICLE_DATA) {
        if (n->type != (s->format == RTCP_VEHICLE_STRING ? RTCP_OPACK_STRING : RTCP_OPACK_DATA) ||
            n->value.blob.length > RTCP_VEHICLE_MAX_VALUE_BYTES) return false;
        out->as.blob.length = n->value.blob.length;
        memcpy(out->as.blob.bytes, n->value.blob.bytes, n->value.blob.length);
    } else {
        if (n->type != RTCP_OPACK_INT) return false;
        out->as.integer = n->value.integer;
    }
    return valid_value(s, out);
}
/* Parameters are required named fields. A no-argument control accepts null or
 * an empty dictionary as in the reference's image/archive and media commands. */
static bool parse_control(rtcp_vehicle_t *v, size_t slot, const rtcp_opack_node_t *nodes, size_t n, size_t at)
{
    const rtcp_vehicle_control_t *c=&v->controls->entries[slot].spec;
    rtcp_vehicle_control_values_t *args=&v->controls->request[slot];
    memset(args,0,sizeof(*args)); args->count=c->request_count;
    if (!c->request_count && nodes[at].type==RTCP_OPACK_NULL) return true;
    if (nodes[at].type!=RTCP_OPACK_DICT || nodes[at].count!=c->request_count) return false;
    for (size_t i=0;i<c->request_count;++i) {
        size_t value=rtcp_opack_dict_get(nodes,n,at,c->request[i].name);
        rtcp_vehicle_characteristic_t spec=parameter_spec(&c->request[i]);
        if (value==SIZE_MAX || !from_node(&spec,&nodes[value],&args->values[i])) return false;
    }
    return true;
}
static bool valid_control_response(const rtcp_vehicle_control_t *c, const rtcp_vehicle_control_values_t *values)
{
    if (values->count!=c->response_count) return false;
    for (size_t i=0;i<values->count;++i) {
        rtcp_vehicle_characteristic_t spec=parameter_spec(&c->response[i]);
        if (!valid_value(&spec,&values->values[i])) return false;
    }
    return true;
}
static void control_response(builder_t *b, const rtcp_vehicle_control_t *c, const rtcp_vehicle_control_values_t *values)
{
    if (!values->count) { (void)add(b,RTCP_OPACK_NULL); return; }
    size_t dict=add(b,RTCP_OPACK_DICT);
    for (size_t i=0;i<values->count;++i) { text(b,c->response[i].name); value_node(b,&values->values[i]); }
    finish(b,dict,values->count);
}
typedef enum { CONFIG, READ, REGISTER, WRITE, CONTROL } operation_t;
static int reject(rtcp_vehicle_t *v, int rc) { ++v->stats.rejected_messages; return rc; }

int rtcp_vehicle_handle(rtcp_vehicle_t *v, rtcp_vehicle_lane_t lane,
                        const uint8_t *input, size_t input_length,
                        uint8_t *out, size_t capacity, size_t *length)
{
    if (length) *length = 0;
    if (!v || !input || !out || !length ||
        (lane != RTCP_VEHICLE_NORMAL && lane != RTCP_VEHICLE_HIGH)) return RTCP_EINVAL;
    ++v->stats.requests;
    rtcp_opack_node_t *r = v->request; size_t n;
    if (rtcp_opack_decode(input, input_length, r, RTCP_OPACK_MAX_NODES, &n) != RTCP_OPACK_OK)
        return reject(v, RTCP_EINVAL);
    size_t id = rtcp_opack_dict_get(r, n, 0, "pluginID");
    size_t data = rtcp_opack_dict_get(r, n, 0, "pluginData");
    if (id == SIZE_MAX || data == SIZE_MAX || r[id].type != RTCP_OPACK_INT ||
        r[data].type != RTCP_OPACK_DATA) return reject(v, RTCP_EINVAL);
    if (r[id].value.integer != v->plugin_id) return reject(v, RTCP_ENOENT);
    const uint8_t *inner = r[data].value.blob.bytes; size_t inner_length = r[data].value.blob.length;
    if (rtcp_opack_decode(inner, inner_length, r, RTCP_OPACK_MAX_NODES, &n) != RTCP_OPACK_OK)
        return reject(v, RTCP_EINVAL);
    size_t cmd = rtcp_opack_dict_get(r, n, 0, "command");
    size_t transaction = rtcp_opack_dict_get(r, n, 0, "transactionID");
    size_t values = rtcp_opack_dict_get(r, n, 0, "values");
    if (cmd == SIZE_MAX || transaction == SIZE_MAX ||
        (r[transaction].type != RTCP_OPACK_INT && r[transaction].type != RTCP_OPACK_NULL))
        return reject(v, RTCP_EINVAL);
    operation_t op; const char *response;
    if (named(&r[cmd], "configRequest")) { op = CONFIG; response = "configResponse"; }
    else if (named(&r[cmd], "readRequest")) { op = READ; response = "readResponse"; }
    else if (named(&r[cmd], "registerRequest")) { op = REGISTER; response = "registerResponse"; }
    else if (named(&r[cmd], "writeRequest")) { op = WRITE; response = "writeResponse"; }
    else if (named(&r[cmd], "controlRequest")) { op = CONTROL; response = "controlResponse"; }
    else return reject(v, RTCP_ENOTSUP);
    size_t items = 0; size_t indices[MAX]; int64_t ids[MAX]; int64_t errors[MAX];
    bool accepted[MAX] = {false};
    if (op != CONFIG) {
        bool dict = op == WRITE || op == CONTROL;
        if (values == SIZE_MAX || r[values].type != (dict ? RTCP_OPACK_DICT : RTCP_OPACK_ARRAY) ||
            r[values].count > MAX) return reject(v, RTCP_EINVAL);
        items = r[values].count; size_t at = values + 1;
        for (size_t i = 0; i < items; ++i) {
            if (r[at].type != RTCP_OPACK_INT) return reject(v, RTCP_EINVAL);
            ids[i] = r[at].value.integer;
            for (size_t j = 0; j < i; ++j) if (ids[j] == ids[i]) return reject(v, RTCP_EINVAL);
            at = r[at].end; indices[i] = at;
            if (dict) at = r[at].end;
        }
    }
    /* Validate every registered command before any host preparation. */
    if (op==CONTROL) for (size_t i=0;i<items;++i) {
        size_t slot=find_control(v,ids[i]);
        if (slot!=SIZE_MAX && !parse_control(v,slot,r,n,indices[i])) return reject(v,RTCP_EINVAL);
    }
    memcpy(v->staged, v->entries, v->count * sizeof(entry_t));
    for (size_t i = 0; i < items; ++i) {
        size_t e = find(v, ids[i]); errors[i] = NO_VALUE;
        if (op == CONTROL) {
            errors[i]=NO_COMMAND;
            size_t slot=find_control(v,ids[i]);
            if (slot==SIZE_MAX) continue;
            rtcp_vehicle_control_values_t *result=&v->controls->response[slot];
            memset(result,0,sizeof(*result));
            if (!v->controls->prepare(v->context,ids[i],&v->controls->request[slot],result))
                return reject(v,RTCP_ESTATE);
            if (!valid_control_response(&v->controls->entries[slot].spec,result)) return reject(v,RTCP_EINVAL);
            accepted[i]=true; continue;
        }
        if (e == SIZE_MAX) continue;
        entry_t *entry = &v->staged[e];
        if (op == REGISTER) entry->subscribed = true;
        if (op == READ || op == REGISTER) { accepted[i] = entry->has_value; continue; }
        rtcp_vehicle_value_t value;
        if (!entry->spec.mutable_value || entry->spec.owner == RTCP_VEHICLE_CAR ||
            !from_node(&entry->spec, &r[indices[i]], &value) ||
            (v->validate_write && !v->validate_write(v->context, ids[i], &value))) continue;
        set(entry, &value); accepted[i] = true; errors[i] = 0;
    }
    builder_t *b = &v->builder; b->used = 0; b->failed = false;
    size_t root = add(b, RTCP_OPACK_DICT);
    text(b, "command"); text(b, response);
    text(b, "transactionID");
    if (r[transaction].type == RTCP_OPACK_INT) integer(b, r[transaction].value.integer);
    else (void)add(b, RTCP_OPACK_NULL);
    size_t pairs = 2;
    if (op == CONFIG) {
        text(b, "values"); size_t val = add(b, RTCP_OPACK_DICT);
        text(b, "accessories"); accessories(v, b); finish(b, val, 1); ++pairs;
    } else {
        if (op == READ || op == REGISTER) {
            text(b, "values"); size_t vals = add(b, RTCP_OPACK_DICT), count = 0;
            for (size_t i = 0; i < items; ++i) if (accepted[i]) {
                integer(b, ids[i]); value_node(b, &v->staged[find(v, ids[i])].value); ++count;
            }
            finish(b, vals, count); ++pairs;
        }
        if (op==CONTROL) {
            size_t count=0;
            for (size_t i=0;i<items;++i) count+=accepted[i];
            if (count) {
                text(b,"values"); size_t vals=add(b,RTCP_OPACK_DICT);
                for (size_t i=0;i<items;++i) if (accepted[i]) {
                    size_t slot=find_control(v,ids[i]); integer(b,ids[i]);
                    control_response(b,&v->controls->entries[slot].spec,&v->controls->response[slot]);
                }
                finish(b,vals,count); ++pairs;
            }
        }
        text(b, "errors"); size_t errs = add(b, RTCP_OPACK_DICT), count = 0;
        for (size_t i = 0; i < items; ++i) if (op == WRITE || !accepted[i]) {
            integer(b, ids[i]); integer(b, errors[i]); ++count;
        }
        finish(b, errs, count); ++pairs;
    }
    finish(b, root, pairs);
    int rc = wrap(v, out, capacity, length);
    if (rc != RTCP_OK) return rc;
    memcpy(v->entries, v->staged, v->count * sizeof(entry_t));
    if (op == WRITE) for (size_t i = 0; i < items; ++i) {
        if (accepted[i]) {
            ++v->stats.committed_writes;
            if (v->did_write) v->did_write(v->context, ids[i], &v->entries[find(v, ids[i])].value);
        } else ++v->stats.refused_writes;
    }
    if (op==CONTROL) for (size_t i=0;i<items;++i) if (accepted[i]) {
        ++v->stats.committed_controls;
        if (v->controls->commit) v->controls->commit(v->context,ids[i],&v->controls->request[find_control(v,ids[i])]);
    }
    return RTCP_OK;
}
int rtcp_vehicle_notification(rtcp_vehicle_t *v, rtcp_vehicle_lane_t *lane,
                              uint8_t *out, size_t capacity, size_t *length)
{
    if (length) *length = 0;
    if (!v || !lane || !out || !length) return RTCP_EINVAL;
    size_t count = 0;
    for (size_t i = 0; i < v->count; ++i) if (v->entries[i].dirty) ++count;
    if (!count) return RTCP_ENOENT;
    builder_t *b = &v->builder; b->used = 0; b->failed = false;
    size_t root = add(b, RTCP_OPACK_DICT);
    text(b, "command"); text(b, "updateNotify");
    text(b, "values"); size_t vals = add(b, RTCP_OPACK_DICT);
    for (size_t i = 0; i < v->count; ++i) if (v->entries[i].dirty) {
        integer(b, v->entries[i].spec.iid); value_node(b, &v->entries[i].value);
    }
    finish(b, vals, count); finish(b, root, 2);
    int rc = wrap(v, out, capacity, length);
    if (rc != RTCP_OK) return rc;
    for (size_t i = 0; i < v->count; ++i) v->entries[i].dirty = false;
    *lane = RTCP_VEHICLE_HIGH; ++v->stats.notifications;
    return RTCP_OK;
}
