/* SPDX-License-Identifier: GPL-3.0-or-later
 * Initial synthetic scenario from LIVI ui_sync.rs tests; see NOTICE.
 */
#include "rtcp/ui_sync.h"
#include "rtcp/rtcp.h"
#include "airplay_bplist_value.h"
#include "ui_sync_test_fixtures.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool dump;
static uint8_t last[RTCP_UI_SYNC_MAX_MESSAGE];
static rtcp_opack_node_t nodes[512];
static airplay_bplist_doc_t header_doc, command_doc;
static airplay_bplist_object_t header_root, command_root;
#define HANDLE(s,name) rtcp_ui_sync_handle(s,ui_fixtures[UI_FIXTURE_##name].bytes,ui_fixtures[UI_FIXTURE_##name].length)
static void hex(const char *prefix, const uint8_t *bytes, size_t length)
{
    if (!dump) return;
    fputs(prefix,stdout); for (size_t i=0;i<length;++i) printf("%02x",bytes[i]); putchar('\n');
}
static uint64_t value(const airplay_bplist_doc_t *d, airplay_bplist_object_t root, const char *key)
{
    airplay_bplist_object_t o; uint64_t n;
    assert(airplay_bplist_dict_get_unique(d,root,key,&o) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(d,o,&n) == AIRPLAY_BPLIST_OK); return n;
}
static airplay_bplist_object_t object(const airplay_bplist_doc_t *d, airplay_bplist_object_t root, const char *key)
{
    airplay_bplist_object_t o;
    assert(airplay_bplist_dict_get_unique(d,root,key,&o) == AIRPLAY_BPLIST_OK); return o;
}
static void string_is(const airplay_bplist_doc_t *d, airplay_bplist_object_t root, const char *key, const char *expected)
{
    airplay_bplist_object_t o=object(d,root,key); char text[256]; size_t length;
    assert(airplay_bplist_get_string(d,o,text,sizeof(text),&length) == AIRPLAY_BPLIST_OK);
    assert(length == strlen(expected) && !strcmp(text,expected));
}
static bool bool_value(const airplay_bplist_doc_t *d, airplay_bplist_object_t root, const char *key)
{
    bool b; airplay_bplist_object_t o=object(d,root,key);
    assert(airplay_bplist_get_bool(d,o,&b) == AIRPLAY_BPLIST_OK); return b;
}
static rtcp_ui_sync_status_t status(rtcp_ui_sync_t *s)
{ rtcp_ui_sync_status_t st; assert(rtcp_ui_sync_status(s,&st) == RTCP_OK); return st; }
static void take(rtcp_ui_sync_t *s, uint64_t type, uint64_t psn, uint64_t asn)
{
    const uint8_t *bytes; size_t length,count;
    assert(rtcp_ui_sync_peek(s,&bytes,&length) == RTCP_OK && length<=sizeof(last));
    memcpy(last,bytes,length); hex("UIHEX ",last,length);
    assert(rtcp_ui_sync_consume(s) == RTCP_OK);
    assert(rtcp_opack_decode(last,length,nodes,512,&count) == RTCP_OPACK_OK);
    size_t display=rtcp_opack_dict_get(nodes,count,0,"displayUUID"); assert(display != SIZE_MAX);
    assert(nodes[display].value.blob.length == strlen("Cluster_Display"));
    assert(!memcmp(nodes[display].value.blob.bytes,"Cluster_Display",strlen("Cluster_Display")));
    size_t data=rtcp_opack_dict_get(nodes,count,0,"data"); assert(data != SIZE_MAX);
    assert(airplay_bplist_open(nodes[data].value.blob.bytes,nodes[data].value.blob.length,&header_doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&header_doc,&header_root) == AIRPLAY_BPLIST_OK);
    assert(value(&header_doc,header_root,"type") == type);
    assert(value(&header_doc,header_root,"ssn") == 1);
    assert(value(&header_doc,header_root,"psn") == psn);
    assert(value(&header_doc,header_root,"asn") == asn);
}
static airplay_bplist_object_t command_is(uint64_t type)
{
    airplay_bplist_object_t o=object(&header_doc,header_root,"data"); const uint8_t *bytes; size_t length;
    assert(airplay_bplist_get_data(&header_doc,o,&bytes,&length) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_open(bytes,length,&command_doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&command_doc,&command_root) == AIRPLAY_BPLIST_OK);
    assert(value(&command_doc,command_root,"type") == type);
    return object(&command_doc,command_root,"data");
}
static rtcp_ui_sync_t *create(bool focus)
{
    char display[]="Cluster_Display";
    rtcp_ui_sync_config_t config={.struct_size=sizeof(config),.display_uuid=display,
        .allow_layout_change=true,.give_focus=focus,.focus_token=12345};
    rtcp_ui_sync_t *s=NULL; assert(rtcp_ui_sync_create(&config,&s) == RTCP_OK && s);
    memset(display,0,sizeof(display)); return s;
}
static void want_layout(rtcp_ui_sync_t *s)
{
    char id[]="0D38B526", palette[]="blue", zone[]="F0786093", component[]="2B28C705";
    rtcp_ui_sync_component_t active={zone,component};
    rtcp_ui_sync_layout_t layout={id,palette,&active,1};
    assert(rtcp_ui_sync_select(s,&layout) == RTCP_OK);
    memset(id,0,sizeof(id)); memset(palette,0,sizeof(palette)); memset(zone,0,sizeof(zone)); memset(component,0,sizeof(component));
}
static void take_complete(rtcp_ui_sync_t *s, unsigned version, bool layout, bool focus)
{
    take(s,1,2,1); assert(value(&header_doc,header_root,"version") == version);
    airplay_bplist_object_t id;
    if (version == 1) assert(airplay_bplist_dict_get_unique(&header_doc,header_root,"id",&id) == AIRPLAY_BPLIST_NOT_FOUND);
    else string_is(&header_doc,header_root,"id","89858A51");
    take(s,2,3,1); airplay_bplist_object_t args=command_is(0x11);
    assert(bool_value(&command_doc,args,"allow"));
    uint64_t next=4;
    if (layout) {
        take(s,2,next++,1); args=command_is(5);
        string_is(&command_doc,args,"layoutID","0D38B526"); string_is(&command_doc,args,"paletteID","blue");
        assert(bool_value(&command_doc,args,"fadeInNewLayout") && bool_value(&command_doc,args,"fadeOutOldLayout"));
        airplay_bplist_object_t active=object(&command_doc,args,"activeComponents");
        string_is(&command_doc,active,"F0786093","2B28C705");
    }
    if (focus) { take(s,2,next,1); args=command_is(7); assert(value(&command_doc,args,"tokenID") == 12345); }
    assert(!status(s).pending_outputs);
}
static void main_scenario(void)
{
    rtcp_ui_sync_t *s=create(true); want_layout(s);
    assert(HANDLE(s,reset) == RTCP_OK && status(s).state == RTCP_UI_SYNC_RESETTING);
    const uint8_t *first,*second; size_t n,m;
    assert(rtcp_ui_sync_peek(s,&first,&n) == RTCP_OK);
    assert(rtcp_ui_sync_peek(s,&second,&m) == RTCP_OK && first==second && n==m && !memcmp(first,second,n));
    assert(HANDLE(s,complete) == RTCP_ESTATE && status(s).next_tx == 2);
    assert(rtcp_ui_sync_set_dark(s,true) == RTCP_ESTATE && !status(s).requested_dark);
    take(s,0,1,0); assert(value(&header_doc,header_root,"version") == 2);
    string_is(&header_doc,header_root,"id","89858A51");
    assert(HANDLE(s,reset) == RTCP_OK && !status(s).pending_outputs && status(s).duplicates == 1);
    assert(HANDLE(s,version_zero) == RTCP_EINVAL && status(s).state == RTCP_UI_SYNC_RESETTING);
    assert(HANDLE(s,complete) == RTCP_OK && status(s).pending_outputs == 4);
    take_complete(s,2,true,true);
    assert(status(s).state == RTCP_UI_SYNC_READY && status(s).next_tx == 6);
    assert(HANDLE(s,basic) == RTCP_OK); take(s,3,6,2);
    assert(HANDLE(s,basic) == RTCP_OK); take(s,3,7,2); assert(status(s).duplicates == 2);
    assert(HANDLE(s,layout) == RTCP_OK); take(s,3,8,3);
    assert(!strcmp(status(s).phone_layout,"DB7C6944") && !strcmp(status(s).phone_palette,"dark"));
    assert(HANDLE(s,palette) == RTCP_OK); take(s,3,9,4);
    assert(!strcmp(status(s).phone_palette_layout,"DB7C6944") && !strcmp(status(s).phone_palette,"red"));
    assert(HANDLE(s,appearance) == RTCP_OK); take(s,3,10,5); assert(status(s).phone_appearance == RTCP_UI_SYNC_LIGHT);
    assert(HANDLE(s,focus) == RTCP_OK); take(s,3,11,6); take(s,2,12,6);
    airplay_bplist_object_t args=command_is(7); assert(value(&command_doc,args,"tokenID") == 12345);
    assert(HANDLE(s,no_ack) == RTCP_OK && !status(s).pending_outputs && status(s).phone_appearance == RTCP_UI_SYNC_DARK);
    assert(status(s).phone_choices == 5);
    assert(HANDLE(s,peer_ack) == RTCP_OK && status(s).peer_ack == 5 && status(s).last_rx == 8);
    uint64_t next=status(s).next_tx;
    assert(HANDLE(s,future_ack) == RTCP_EINVAL && status(s).next_tx == next && status(s).last_rx == 8);
    assert(HANDLE(s,stale) == RTCP_OK && status(s).stale == 1 && !status(s).pending_outputs);
    assert(HANDLE(s,unknown) == RTCP_OK); take(s,3,13,10); assert(status(s).ignored == 1);
    assert(HANDLE(s,bad_no_ack) == RTCP_EINVAL && status(s).last_rx == 10);
    assert(HANDLE(s,bad_command) == RTCP_EINVAL && status(s).last_rx == 10);
    assert(rtcp_ui_sync_set_dark(s,true) == RTCP_OK); take(s,2,14,10); args=command_is(5);
    assert(value(&command_doc,args,"appearanceMode") == 1);
    assert(rtcp_ui_sync_set_dark(s,true) == RTCP_OK && !status(s).pending_outputs);
    want_layout(s); take(s,2,15,10);
    assert(rtcp_ui_sync_set_dark(s,false) == RTCP_OK && status(s).pending_outputs == 1);
    rtcp_ui_sync_reset_session(s);
    assert(status(s).state == RTCP_UI_SYNC_IDLE && status(s).abandoned_messages == 1);
    assert(!strcmp(status(s).requested_layout,"0D38B526") && !status(s).phone_layout[0]);
    assert(!status(s).requested_dark && !status(s).pending_outputs && !status(s).session);
    assert(HANDLE(s,reset) == RTCP_OK); take(s,0,1,0);
    assert(HANDLE(s,complete_v1) == RTCP_OK); take_complete(s,1,true,true);
    rtcp_ui_sync_destroy(s);
}
static void identifiers_and_errors(void)
{
    rtcp_ui_sync_t *s=create(false);
    assert(HANDLE(s,wrong_display) == RTCP_ENOENT && !status(s).session);
    assert(HANDLE(s,duplicate_type) == RTCP_EINVAL && !status(s).session);
    assert(HANDLE(s,large_panel) == RTCP_ENOMEM && !status(s).session);
    assert(HANDLE(s,dense_panel) == RTCP_ENOMEM && !status(s).session && !status(s).pending_outputs);
    assert(HANDLE(s,id_array) == RTCP_ENOTSUP && !status(s).session);
    assert(HANDLE(s,id_unicode) == RTCP_EINVAL && !status(s).session);
    const unsigned ids[]={UI_FIXTURE_id_data,UI_FIXTURE_id_bool,UI_FIXTURE_id_negative,UI_FIXTURE_id_null};
    for (unsigned i=0;i<4;++i) {
        assert(rtcp_ui_sync_handle(s,ui_fixtures[ids[i]].bytes,ui_fixtures[ids[i]].length) == RTCP_OK);
        take(s,0,1,0); airplay_bplist_object_t id=object(&header_doc,header_root,"id"); airplay_bplist_type_t type;
        assert(airplay_bplist_get_type(&header_doc,id,&type,NULL) == AIRPLAY_BPLIST_OK);
        if (i == 0) {
            const uint8_t *bytes; size_t length;
            assert(type == AIRPLAY_BPLIST_DATA && airplay_bplist_get_data(&header_doc,id,&bytes,&length) == AIRPLAY_BPLIST_OK);
            assert(length == 8 && !memcmp(bytes,"phone-id",8));
        } else if (i == 1) assert(type == AIRPLAY_BPLIST_BOOL && bool_value(&header_doc,header_root,"id"));
        else if (i == 2) assert(type == AIRPLAY_BPLIST_INTEGER && value(&header_doc,header_root,"id") == (uint64_t)-7);
        else assert(type == AIRPLAY_BPLIST_NULL);
        rtcp_ui_sync_reset_session(s);
    }
    for (size_t i=0;i<ui_fixtures[UI_FIXTURE_reset].length;++i)
        assert(rtcp_ui_sync_handle(s,ui_fixtures[UI_FIXTURE_reset].bytes,i) != RTCP_OK && !status(s).session);
    uint8_t bytes[128]; uint32_t seed=12345;
    for (unsigned i=1;i<=1000;++i) {
        size_t n=i%sizeof(bytes)+1;
        for (size_t j=0;j<n;++j) { seed=seed*1664525U+1013904223U; bytes[j]=(uint8_t)(seed>>24); }
        (void)rtcp_ui_sync_handle(s,bytes,n); assert(!status(s).session && !status(s).pending_outputs);
    }
    assert(HANDLE(s,reset) == RTCP_OK); take(s,0,1,0);
    assert(HANDLE(s,complete) == RTCP_OK); take_complete(s,2,false,false);
    assert(HANDLE(s,focus) == RTCP_OK); take(s,3,4,6); assert(!status(s).pending_outputs);
    rtcp_ui_sync_component_t duplicated[]={{"zone","a"},{"zone","b"}};
    rtcp_ui_sync_layout_t bad={"layout","palette",duplicated,2};
    assert(rtcp_ui_sync_select(s,&bad) == RTCP_EINVAL && !status(s).requested_layout[0]);
    bad.active_count=9; assert(rtcp_ui_sync_select(s,&bad) == RTCP_EINVAL);
    assert(rtcp_ui_sync_consume(s) == RTCP_ENOENT);
    rtcp_ui_sync_destroy(s);
    rtcp_ui_sync_config_t config={.struct_size=sizeof(config),.display_uuid="display",.give_focus=true,.focus_token=UINT64_MAX};
    assert(rtcp_ui_sync_create(&config,&s) == RTCP_EINVAL && !s);
    config.give_focus=false; config.display_uuid="\xc3\xa9";
    assert(rtcp_ui_sync_create(&config,&s) == RTCP_EINVAL && !s);
    assert(rtcp_ui_sync_status(NULL,NULL) == RTCP_EINVAL);
    rtcp_ui_sync_reset_session(NULL); rtcp_ui_sync_destroy(NULL);
}
static size_t used;
static size_t node(rtcp_opack_type_t type)
{ size_t at=used++; nodes[at]=(rtcp_opack_node_t){.type=type,.end=(uint16_t)used}; return at; }
static void text(const char *s)
{ size_t at=node(RTCP_OPACK_STRING); nodes[at].value.blob.bytes=(const uint8_t *)s; nodes[at].value.blob.length=strlen(s); }
static void codec_limits(void)
{
    used=0; node(RTCP_OPACK_DICT);
    text("null"); node(RTCP_OPACK_NULL);
    text("true"); nodes[node(RTCP_OPACK_BOOL)].value.boolean=true;
    text("false"); node(RTCP_OPACK_BOOL);
    text("negative"); nodes[node(RTCP_OPACK_INT)].value.integer=-7;
    text("data"); size_t at=node(RTCP_OPACK_DATA); static const uint8_t data[]={0,1,255};
    nodes[at].value.blob.bytes=data; nodes[at].value.blob.length=sizeof(data);
    text("items"); size_t array=node(RTCP_OPACK_ARRAY);
    for (unsigned i=0;i<16;++i) nodes[node(RTCP_OPACK_INT)].value.integer=i;
    nodes[array].count=16; nodes[array].end=(uint16_t)used;
    nodes[0].count=6; nodes[0].end=(uint16_t)used;
    size_t length;
    assert(airplay_bplist_encode_nodes(nodes,used,last,sizeof(last),&length) == AIRPLAY_BPLIST_OK);
    hex("PLISTHEX ",last,length);
    for (size_t i=0;i<length;++i) {
        size_t failed=123; assert(airplay_bplist_encode_nodes(nodes,used,last,i,&failed) == AIRPLAY_BPLIST_OUTPUT_TOO_SMALL && !failed);
    }
    nodes[0].end=1; assert(airplay_bplist_encode_nodes(nodes,used,last,sizeof(last),&length) != AIRPLAY_BPLIST_OK && !length);
    nodes[0]=(rtcp_opack_node_t){.type=RTCP_OPACK_STRING,.end=1};
    nodes[0].value.blob.bytes=(const uint8_t *)"\xc3\xa9"; nodes[0].value.blob.length=2;
    assert(airplay_bplist_encode_nodes(nodes,1,last,sizeof(last),&length) == AIRPLAY_BPLIST_INVALID_OBJECT);
    nodes[0].type=RTCP_OPACK_REAL;
    assert(airplay_bplist_encode_nodes(nodes,1,last,sizeof(last),&length) == AIRPLAY_BPLIST_INVALID_OBJECT);
    uint8_t large[512]; memset(large,0xa5,sizeof(large));
    nodes[0]=(rtcp_opack_node_t){.type=RTCP_OPACK_DATA,.end=1};
    nodes[0].value.blob.bytes=large; nodes[0].value.blob.length=sizeof(large);
    assert(airplay_bplist_encode_nodes(nodes,1,last,sizeof(last),&length) == AIRPLAY_BPLIST_OK && last[length-26] == 2);
    airplay_bplist_doc_t doc; airplay_bplist_object_t root; const uint8_t *blob; size_t size;
    assert(airplay_bplist_open(last,length,&doc) == AIRPLAY_BPLIST_OK && airplay_bplist_root(&doc,&root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_data(&doc,root,&blob,&size) == AIRPLAY_BPLIST_OK && size==sizeof(large) && !memcmp(blob,large,size));
    for (unsigned i=0;i<18;++i) nodes[i]=(rtcp_opack_node_t){.type=RTCP_OPACK_ARRAY,.count=1,.end=19};
    nodes[18]=(rtcp_opack_node_t){.type=RTCP_OPACK_NULL,.end=19};
    assert(airplay_bplist_encode_nodes(nodes,19,last,sizeof(last),&length) == AIRPLAY_BPLIST_INVALID_OBJECT);
}
int main(int argc, char **argv)
{
    dump=argc==2 && !strcmp(argv[1],"--dump"); assert(argc==1 || dump);
    main_scenario(); identifiers_and_errors(); codec_limits();
    puts("UI Sync: independent input, reset/version/layout/palette/appearance/focus/ACK, duplicates, retained output, reset and malformed bounds passed");
    return 0;
}
