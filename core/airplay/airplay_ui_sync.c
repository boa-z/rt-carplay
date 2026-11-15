/* SPDX-License-Identifier: GPL-3.0-or-later
 * Based on LIVI ui_sync.rs by Lasse Heitgres; see NOTICE. This bounded state
 * machine does not supply theme assets, rendering or presentation clocks.
 */
#include "rtcp/ui_sync.h"
#include "rtcp/opack.h"
#include "rtcp/rtcp.h"
#include "airplay_bplist_value.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define ID (RTCP_UI_SYNC_MAX_ID+1)
#define NODES AIRPLAY_BPLIST_VALUE_MAX_NODES
#define PANEL_BYTES 1024U
#define PHONE_ID_BYTES 256U
typedef struct { char zone[ID], component[ID]; } component_t;
typedef struct {
    rtcp_ui_sync_status_t status;
    char wanted[ID], palette[ID]; component_t active[RTCP_UI_SYNC_MAX_ACTIVE]; size_t active_count;
    uint8_t panel[PANEL_BYTES]; size_t panel_length;
    bool has_phone_id; rtcp_opack_type_t phone_id_type;
    uint8_t phone_id[PHONE_ID_BYTES]; size_t phone_id_length;
    int64_t phone_integer; bool phone_boolean;
} state_t;
typedef struct { size_t length; uint8_t bytes[RTCP_UI_SYNC_MAX_MESSAGE]; } output_t;
struct rtcp_ui_sync {
    char display[ID]; bool allow_layout_change, give_focus; int64_t focus_token;
    state_t current, work;
    output_t outputs[RTCP_UI_SYNC_MAX_OUTPUTS]; size_t head, pending;
    rtcp_opack_node_t nodes[NODES], frame[RTCP_OPACK_MAX_NODES]; size_t used;
    uint8_t command[4096], header[4096];
    bool builder_failed;
};
static bool identifier(const char *s)
{
    if (!s || !*s) return false;
    size_t n=0;
    while (n<ID && s[n]) { if ((unsigned char)s[n] > 127) return false; ++n; }
    return n<ID;
}
static size_t add(rtcp_ui_sync_t *s, rtcp_opack_type_t type)
{
    if (s->used >= NODES) { s->builder_failed=true; return NODES-1; }
    size_t i=s->used++;
    s->nodes[i]=(rtcp_opack_node_t){.type=type,.end=(uint16_t)s->used}; return i;
}
static void begin(rtcp_ui_sync_t *s) { s->used=0; s->builder_failed=false; (void)add(s,RTCP_OPACK_DICT); }
static void text(rtcp_ui_sync_t *s, const char *str)
{
    size_t i=add(s,RTCP_OPACK_STRING); s->nodes[i].value.blob.bytes=(const uint8_t *)str;
    s->nodes[i].value.blob.length=strlen(str);
}
static void number(rtcp_ui_sync_t *s, int64_t n) { s->nodes[add(s,RTCP_OPACK_INT)].value.integer=n; }
static void boolean(rtcp_ui_sync_t *s, bool b) { s->nodes[add(s,RTCP_OPACK_BOOL)].value.boolean=b; }
static void data(rtcp_ui_sync_t *s, const uint8_t *bytes, size_t length)
{
    size_t i=add(s,RTCP_OPACK_DATA); s->nodes[i].value.blob.bytes=bytes; s->nodes[i].value.blob.length=length;
}
static void finish(rtcp_ui_sync_t *s, size_t at, size_t count)
{ s->nodes[at].count=(uint16_t)count; s->nodes[at].end=(uint16_t)s->used; }
static int plist(rtcp_ui_sync_t *s, uint8_t *out, size_t cap, size_t *length)
{
    if (s->builder_failed) return RTCP_ENOMEM;
    airplay_bplist_result_t rc=airplay_bplist_encode_nodes(s->nodes,s->used,out,cap,length);
    return rc == AIRPLAY_BPLIST_OK ? RTCP_OK : rc == AIRPLAY_BPLIST_OUTPUT_TOO_SMALL ? RTCP_ENOMEM : RTCP_EINVAL;
}
static void phone_id(rtcp_ui_sync_t *s)
{
    state_t *w=&s->work; text(s,"id"); size_t at=add(s,w->phone_id_type);
    if (w->phone_id_type == RTCP_OPACK_INT) s->nodes[at].value.integer=w->phone_integer;
    else if (w->phone_id_type == RTCP_OPACK_BOOL) s->nodes[at].value.boolean=w->phone_boolean;
    else if (w->phone_id_type == RTCP_OPACK_DATA || w->phone_id_type == RTCP_OPACK_STRING) {
        s->nodes[at].value.blob.bytes=w->phone_id; s->nodes[at].value.blob.length=w->phone_id_length;
    }
}
static int copy_subtree(rtcp_ui_sync_t *s, const rtcp_opack_node_t *src, size_t count, size_t at)
{
    if (at >= count || src[at].end <= at || src[at].end > count) return RTCP_EINVAL;
    size_t end=src[at].end, base=s->used;
    if (end-at > NODES-s->used) return RTCP_ENOMEM;
    for (size_t j=at;j<end;++j) {
        s->nodes[s->used]=src[j];
        s->nodes[s->used++].end=(uint16_t)(base+src[j].end-at);
    }
    return RTCP_OK;
}
static int emit(rtcp_ui_sync_t *s, unsigned type, const uint8_t *command, size_t command_length,
    bool version, bool with_id)
{
    state_t *w=&s->work; rtcp_ui_sync_status_t *st=&w->status;
    if (s->pending >= RTCP_UI_SYNC_MAX_OUTPUTS || st->next_tx >= INT64_MAX) return RTCP_ESTATE;
    begin(s); text(s,"type"); number(s,type); text(s,"ssn"); number(s,(int64_t)st->session);
    text(s,"psn"); number(s,(int64_t)st->next_tx); text(s,"asn"); number(s,(int64_t)st->last_rx);
    size_t pairs=4;
    if (version) { text(s,"version"); number(s,st->version); ++pairs; }
    if (with_id && w->has_phone_id) { phone_id(s); ++pairs; }
    if (command) { text(s,"data"); data(s,command,command_length); ++pairs; }
    finish(s,0,pairs); size_t header_length;
    int rc=plist(s,s->header,sizeof(s->header),&header_length); if (rc) return rc;
    size_t count;
    if (rtcp_opack_decode(w->panel,w->panel_length,s->frame,RTCP_OPACK_MAX_NODES,&count) != RTCP_OPACK_OK) return RTCP_EINVAL;
    begin(s);
    size_t at=1;
    for (unsigned i=0;i<s->frame[0].count*2U;++i) {
        rc=copy_subtree(s,s->frame,count,at); if (rc) return rc; at=s->frame[at].end;
    }
    text(s,"data"); data(s,s->header,header_length); finish(s,0,s->frame[0].count+1U);
    if (s->builder_failed) return RTCP_ENOMEM;
    output_t *out=&s->outputs[s->pending];
    if (rtcp_opack_encode(s->nodes,s->used,out->bytes,sizeof(out->bytes),&out->length) != RTCP_OPACK_OK) return RTCP_ENOMEM;
    ++s->pending; ++st->next_tx; ++st->generated_messages; return RTCP_OK;
}
static int command(rtcp_ui_sync_t *s, unsigned kind)
{
    state_t *w=&s->work; begin(s); text(s,"type"); number(s,kind); text(s,"data");
    size_t d=add(s,RTCP_OPACK_DICT), pairs=1;
    if (kind == 0x11) { text(s,"allow"); boolean(s,s->allow_layout_change); }
    else if (kind == 7) { text(s,"tokenID"); number(s,s->focus_token); }
    else if (kind == 5) {
        text(s,"layoutID"); text(s,w->wanted); text(s,"paletteID"); text(s,w->palette);
        text(s,"appearanceMode"); number(s,w->status.requested_dark ? 1 : 0);
        text(s,"fadeInNewLayout"); boolean(s,true); text(s,"fadeOutOldLayout"); boolean(s,true);
        text(s,"activeComponents"); size_t active=add(s,RTCP_OPACK_DICT);
        for (size_t i=0;i<w->active_count;++i) { text(s,w->active[i].zone); text(s,w->active[i].component); }
        finish(s,active,w->active_count); pairs=6;
    } else return RTCP_ENOTSUP;
    finish(s,d,pairs); finish(s,0,2); size_t length;
    int rc=plist(s,s->command,sizeof(s->command),&length);
    return rc ? rc : emit(s,2,s->command,length,false,false);
}
static int fail(rtcp_ui_sync_t *s, int rc)
{ s->pending=0; ++s->current.status.rejected; s->current.status.last_error=rc; return rc; }
static int commit(rtcp_ui_sync_t *s)
{ s->work.status.last_error=0; s->current=s->work; s->head=0; return RTCP_OK; }
int rtcp_ui_sync_create(const rtcp_ui_sync_config_t *config, rtcp_ui_sync_t **out)
{
    if (!out) return RTCP_EINVAL;
    *out=NULL;
    if (!config || config->struct_size != sizeof(*config) || !identifier(config->display_uuid) ||
        (config->give_focus && config->focus_token > INT64_MAX)) return RTCP_EINVAL;
    rtcp_ui_sync_t *s=calloc(1,sizeof(*s)); if (!s) return RTCP_ENOMEM;
    strcpy(s->display,config->display_uuid); s->allow_layout_change=config->allow_layout_change;
    s->give_focus=config->give_focus; s->focus_token=config->give_focus ? (int64_t)config->focus_token : 0;
    s->current.status.requested_dark=config->initial_dark; *out=s; return RTCP_OK;
}
void rtcp_ui_sync_destroy(rtcp_ui_sync_t *s)
{ if (s) { memset(s,0,sizeof(*s)); free(s); } }
void rtcp_ui_sync_reset_session(rtcp_ui_sync_t *s)
{
    if (!s) return;
    rtcp_ui_sync_status_t old=s->current.status;
    old.abandoned_messages+=s->pending;
    old.state=RTCP_UI_SYNC_IDLE; old.session=old.next_tx=old.last_rx=old.peer_ack=0; old.version=0;
    old.pending_outputs=0; old.last_error=0;
    old.phone_layout[0]=old.phone_palette[0]=old.phone_palette_layout[0]=0; old.phone_appearance=RTCP_UI_SYNC_AUTOMATIC;
    s->current.status=old; s->current.panel_length=0; s->current.has_phone_id=false;
    s->pending=s->head=0;
}
int rtcp_ui_sync_status(const rtcp_ui_sync_t *s, rtcp_ui_sync_status_t *out)
{
    if (!s || !out) return RTCP_EINVAL;
    *out=s->current.status; out->pending_outputs=s->pending;
    strcpy(out->requested_layout,s->current.wanted); strcpy(out->requested_palette,s->current.palette);
    return RTCP_OK;
}
int rtcp_ui_sync_select(rtcp_ui_sync_t *s, const rtcp_ui_sync_layout_t *layout)
{
    if (!s || !layout || !identifier(layout->id) || !identifier(layout->palette) ||
        layout->active_count > RTCP_UI_SYNC_MAX_ACTIVE || (!layout->active && layout->active_count)) return RTCP_EINVAL;
    if (s->pending) return RTCP_ESTATE;
    for (size_t i=0;i<layout->active_count;++i) {
        if (!identifier(layout->active[i].zone) || !identifier(layout->active[i].component)) return RTCP_EINVAL;
        for (size_t j=0;j<i;++j) if (!strcmp(layout->active[i].zone,layout->active[j].zone)) return RTCP_EINVAL;
    }
    s->work=s->current; strcpy(s->work.wanted,layout->id); strcpy(s->work.palette,layout->palette);
    s->work.active_count=layout->active_count;
    for (size_t i=0;i<layout->active_count;++i) {
        strcpy(s->work.active[i].zone,layout->active[i].zone); strcpy(s->work.active[i].component,layout->active[i].component);
    }
    int rc=s->work.status.state == RTCP_UI_SYNC_READY ? command(s,5) : RTCP_OK;
    return rc ? fail(s,rc) : commit(s);
}
int rtcp_ui_sync_set_dark(rtcp_ui_sync_t *s, bool dark)
{
    if (!s) return RTCP_EINVAL;
    if (s->pending) return RTCP_ESTATE;
    if (s->current.status.requested_dark == dark) return RTCP_OK;
    s->work=s->current; s->work.status.requested_dark=dark;
    int rc=s->work.status.state == RTCP_UI_SYNC_READY && s->work.wanted[0] ? command(s,5) : RTCP_OK;
    return rc ? fail(s,rc) : commit(s);
}
int rtcp_ui_sync_peek(const rtcp_ui_sync_t *s, const uint8_t **out, size_t *length)
{
    if (out) *out=NULL;
    if (length) *length=0;
    if (!s || !out || !length) return RTCP_EINVAL;
    if (!s->pending) return RTCP_ENOENT;
    *out=s->outputs[s->head].bytes; *length=s->outputs[s->head].length; return RTCP_OK;
}
int rtcp_ui_sync_consume(rtcp_ui_sync_t *s)
{
    if (!s) return RTCP_EINVAL;
    if (!s->pending) return RTCP_ENOENT;
    s->head=(s->head+1)%RTCP_UI_SYNC_MAX_OUTPUTS; --s->pending; ++s->current.status.consumed_messages;
    return RTCP_OK;
}
static int get(const airplay_bplist_doc_t *d, airplay_bplist_object_t root, const char *key,
    airplay_bplist_object_t *out)
{
    airplay_bplist_result_t rc=airplay_bplist_dict_get_unique(d,root,key,out);
    return rc == AIRPLAY_BPLIST_OK ? RTCP_OK : rc == AIRPLAY_BPLIST_NOT_FOUND ? RTCP_ENOENT : RTCP_EINVAL;
}
static int optional_uint(const airplay_bplist_doc_t *d, airplay_bplist_object_t root, const char *key, uint64_t *n)
{
    airplay_bplist_object_t o; int rc=get(d,root,key,&o);
    if (rc == RTCP_ENOENT) return RTCP_OK;
    if (rc || airplay_bplist_get_uint(d,o,n) != AIRPLAY_BPLIST_OK || *n > INT64_MAX) return RTCP_EINVAL;
    return RTCP_OK;
}
static int get_text(const airplay_bplist_doc_t *d, airplay_bplist_object_t root, const char *key, char out[ID])
{
    airplay_bplist_object_t o; int rc=get(d,root,key,&o); if (rc) return rc;
    size_t n;
    if (airplay_bplist_get_string(d,o,out,ID,&n) != AIRPLAY_BPLIST_OK || !identifier(out)) return RTCP_EINVAL;
    return RTCP_OK;
}
static int capture_id(rtcp_ui_sync_t *s, const airplay_bplist_doc_t *d, airplay_bplist_object_t root)
{
    state_t *w=&s->work; airplay_bplist_object_t id; int rc=get(d,root,"id",&id);
    w->has_phone_id=false; if (rc == RTCP_ENOENT) return RTCP_OK; if (rc) return rc;
    airplay_bplist_type_t type; uint64_t count;
    if (airplay_bplist_get_type(d,id,&type,&count) != AIRPLAY_BPLIST_OK) return RTCP_EINVAL;
    switch (type) {
    case AIRPLAY_BPLIST_INTEGER: {
        uint64_t n; if (airplay_bplist_get_uint(d,id,&n) != AIRPLAY_BPLIST_OK) return RTCP_EINVAL;
        /* Preserve all eight wire bytes, including a signed plist integer. */
        memcpy(&w->phone_integer,&n,sizeof(n)); w->phone_id_type=RTCP_OPACK_INT; break;
    }
    case AIRPLAY_BPLIST_BOOL:
        if (airplay_bplist_get_bool(d,id,&w->phone_boolean) != AIRPLAY_BPLIST_OK) return RTCP_EINVAL;
        w->phone_id_type=RTCP_OPACK_BOOL; break;
    case AIRPLAY_BPLIST_NULL: w->phone_id_type=RTCP_OPACK_NULL; break;
    case AIRPLAY_BPLIST_DATA: {
        const uint8_t *bytes; size_t n;
        if (airplay_bplist_get_data(d,id,&bytes,&n) != AIRPLAY_BPLIST_OK || n>sizeof(w->phone_id)) return RTCP_EINVAL;
        memcpy(w->phone_id,bytes,n); w->phone_id_length=n; w->phone_id_type=RTCP_OPACK_DATA; break;
    }
    case AIRPLAY_BPLIST_ASCII_STRING: case AIRPLAY_BPLIST_UNICODE_STRING:
        if (airplay_bplist_get_string(d,id,(char *)w->phone_id,sizeof(w->phone_id),&w->phone_id_length) != AIRPLAY_BPLIST_OK) return RTCP_EINVAL;
        w->phone_id_type=RTCP_OPACK_STRING; break;
    default: return RTCP_ENOTSUP;
    }
    w->has_phone_id=true; return RTCP_OK;
}
static int capture_panel(rtcp_ui_sync_t *s, const uint8_t *input, size_t length,
    const uint8_t **header, size_t *header_length)
{
    size_t count;
    if (rtcp_opack_decode(input,length,s->frame,RTCP_OPACK_MAX_NODES,&count) != RTCP_OPACK_OK) return RTCP_EINVAL;
    size_t d=rtcp_opack_dict_get(s->frame,count,0,"data"), display=rtcp_opack_dict_get(s->frame,count,0,"displayUUID");
    if (d == SIZE_MAX || display == SIZE_MAX || s->frame[d].type != RTCP_OPACK_DATA || s->frame[display].type != RTCP_OPACK_STRING) return RTCP_EINVAL;
    const rtcp_opack_node_t *uuid=&s->frame[display];
    if (uuid->value.blob.length != strlen(s->display) || memcmp(uuid->value.blob.bytes,s->display,uuid->value.blob.length)) return RTCP_ENOENT;
    *header=s->frame[d].value.blob.bytes; *header_length=s->frame[d].value.blob.length;
    begin(s); size_t at=1;
    for (unsigned i=0;i<s->frame[0].count;++i) {
        size_t value=s->frame[at].end;
        if (value != d) {
            int rc=copy_subtree(s,s->frame,count,at); if (rc) return rc;
            rc=copy_subtree(s,s->frame,count,value); if (rc) return rc;
        }
        at=s->frame[value].end;
    }
    finish(s,0,s->frame[0].count-1U);
    if (rtcp_opack_encode(s->nodes,s->used,s->work.panel,sizeof(s->work.panel),&s->work.panel_length) != RTCP_OPACK_OK) return RTCP_ENOMEM;
    return RTCP_OK;
}
static int follow(rtcp_ui_sync_t *s, const airplay_bplist_doc_t *d, airplay_bplist_object_t root, bool *focus)
{
    airplay_bplist_object_t body; int rc=get(d,root,"data",&body);
    if (rc == RTCP_ENOENT) return RTCP_OK;
    if (rc) return rc;
    const uint8_t *bytes; size_t length; airplay_bplist_doc_t command_doc; airplay_bplist_object_t command_root;
    if (airplay_bplist_get_data(d,body,&bytes,&length) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_open(bytes,length,&command_doc) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_root(&command_doc,&command_root) != AIRPLAY_BPLIST_OK) return RTCP_EINVAL;
    uint64_t type=UINT64_MAX; rc=optional_uint(&command_doc,command_root,"type",&type); if (rc || type == UINT64_MAX) return RTCP_EINVAL;
    if (type == 8) { *focus=s->give_focus; return RTCP_OK; }
    if (type != 3 && type != 0x10 && type != 0x13) { ++s->work.status.ignored; return RTCP_OK; }
    airplay_bplist_object_t args; rc=get(&command_doc,command_root,"data",&args); if (rc) return RTCP_EINVAL;
    state_t *w=&s->work;
    if (type == 3) {
        rc=get_text(&command_doc,args,"layoutID",w->status.phone_layout); if (rc) return RTCP_EINVAL;
        ++w->status.phone_choices;
        char palette[ID]; rc=get_text(&command_doc,args,"paletteID",palette);
        if (rc == RTCP_ENOENT) return RTCP_OK;
        if (rc) return rc;
        strcpy(w->status.phone_palette_layout,w->status.phone_layout); strcpy(w->status.phone_palette,palette);
    } else if (type == 0x10) {
        rc=get_text(&command_doc,args,"layoutIdentifier",w->status.phone_palette_layout); if (rc) return RTCP_EINVAL;
        rc=get_text(&command_doc,args,"paletteIdentifier",w->status.phone_palette); if (rc) return RTCP_EINVAL;
    } else {
        uint64_t preference=UINT64_MAX; rc=optional_uint(&command_doc,args,"preference",&preference);
        if (rc || preference>2) return RTCP_EINVAL;
        w->status.phone_appearance=(rtcp_ui_sync_appearance_t)preference;
    }
    ++w->status.phone_choices; return RTCP_OK;
}
int rtcp_ui_sync_handle(rtcp_ui_sync_t *s, const uint8_t *input, size_t length)
{
    if (!s || !input || !length || length>RTCP_UI_SYNC_MAX_MESSAGE) return RTCP_EINVAL;
    if (s->pending) return RTCP_ESTATE;
    s->work=s->current; ++s->current.status.received; ++s->work.status.received;
    const uint8_t *header; size_t header_length;
    int rc=capture_panel(s,input,length,&header,&header_length); if (rc) return fail(s,rc);
    airplay_bplist_doc_t d; airplay_bplist_object_t root;
    if (airplay_bplist_open(header,header_length,&d) != AIRPLAY_BPLIST_OK || airplay_bplist_root(&d,&root) != AIRPLAY_BPLIST_OK) return fail(s,RTCP_EINVAL);
    uint64_t type=UINT64_MAX, ssn=UINT64_MAX, psn=s->work.status.last_rx, asn=s->work.status.peer_ack;
    rc=optional_uint(&d,root,"type",&type); if (rc || type == UINT64_MAX) return fail(s,RTCP_EINVAL);
    rc=optional_uint(&d,root,"ssn",&ssn); if (rc) return fail(s,rc);
    rc=optional_uint(&d,root,"psn",&psn); if (rc) return fail(s,rc);
    rc=optional_uint(&d,root,"asn",&asn); if (rc) return fail(s,rc);
    rtcp_ui_sync_status_t *st=&s->work.status;
    if (type == 0) {
        if (st->state == RTCP_UI_SYNC_RESETTING && st->next_tx == 2) { ++st->duplicates; return commit(s); }
        if (st->session >= INT64_MAX) return fail(s,RTCP_ESTATE);
        ++st->session; st->next_tx=1; st->last_rx=st->peer_ack=0; st->version=2; st->state=RTCP_UI_SYNC_RESETTING;
        rc=capture_id(s,&d,root); if (!rc) rc=emit(s,0,NULL,0,true,true);
    } else if (ssn != st->session || st->state == RTCP_UI_SYNC_IDLE) { ++st->stale; return commit(s); }
    else {
        if (asn >= st->next_tx) return fail(s,RTCP_EINVAL);
        if (asn > st->peer_ack) st->peer_ack=asn;
        if (type == 1 && st->state == RTCP_UI_SYNC_RESETTING) {
            uint64_t version=1; rc=optional_uint(&d,root,"version",&version);
            if (rc || !version) return fail(s,RTCP_EINVAL);
            st->version=version>2 ? 2 : (unsigned)version; st->last_rx=psn; st->state=RTCP_UI_SYNC_READY;
            rc=emit(s,1,NULL,0,true,st->version>=2);
            if (!rc) rc=command(s,0x11);
            if (!rc && s->work.wanted[0]) rc=command(s,5);
            if (!rc && s->give_focus) rc=command(s,7);
        } else if (type == 2 && st->state == RTCP_UI_SYNC_READY) {
            bool no_ack=false, focus=false; airplay_bplist_object_t no;
            rc=get(&d,root,"noAck",&no);
            if (rc == RTCP_OK && airplay_bplist_get_bool(&d,no,&no_ack) != AIRPLAY_BPLIST_OK) return fail(s,RTCP_EINVAL);
            if (rc != RTCP_OK && rc != RTCP_ENOENT) return fail(s,rc);
            rc=RTCP_OK;
            if (psn <= st->last_rx) ++st->duplicates;
            else { rc=follow(s,&d,root,&focus); st->last_rx=psn; }
            if (!rc && !no_ack) rc=emit(s,3,NULL,0,false,false);
            if (!rc && focus) rc=command(s,7);
        } else if (type == 3) { if (psn>st->last_rx) st->last_rx=psn; }
        else ++st->ignored;
    }
    return rc ? fail(s,rc) : commit(s);
}
