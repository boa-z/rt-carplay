/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_info_plist.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rtcp/rtcp.h"

#include "airplay_bplist.h"
#include "airplay_hid_descriptor.h"
#include "audio_codec.h"

static const airplay_info_plist_config_t k_config = {
    .device_id = "00:11:22:33:44:55",
    .bluetooth_id = "00:11:22:33:44:56",
    .name = "Demo CarPlay",
    .model = "D213",
    .manufacturer = "ArtInChip",
    .width_pixels = 1280,
    .height_pixels = 480,
    .width_physical_mm = 300,
    .height_physical_mm = 120,
    .max_fps = 30,
    .right_hand_drive = true,
};

static void expect_string(const airplay_bplist_doc_t *doc,
                          airplay_bplist_object_t dict,
                          const char *key,
                          const char *expected)
{
    airplay_bplist_object_t value;
    char text[128];
    size_t len = 0U;
    assert(airplay_bplist_dict_get(doc, dict, key, &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_string(doc, value, text, sizeof(text), &len) ==
           AIRPLAY_BPLIST_OK);
    assert(strcmp(text, expected) == 0);
    assert(len == strlen(expected));
}

static void expect_uint(const airplay_bplist_doc_t *doc,
                        airplay_bplist_object_t dict,
                        const char *key,
                        uint64_t expected)
{
    airplay_bplist_object_t value;
    uint64_t number = 0U;
    assert(airplay_bplist_dict_get(doc, dict, key, &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(doc, value, &number) == AIRPLAY_BPLIST_OK);
    assert(number == expected);
}

static const char *match_file;
static bool match_expected;
static void test_vehicle_info(bool dump)
{
    /* These synthetic types have no production vehicle meaning. */
    rtcp_vehicle_characteristic_t spec[4]={0};
    for (unsigned i=0;i<4;++i) {
        spec[i]=(rtcp_vehicle_characteristic_t){.accessory_iid=1,.accessory_type=100,
            .service_iid=2,.service_type=200,.iid=3+i,.type=1003+i,
            .owner=RTCP_VEHICLE_CAR,.mutable_value=true,.has_initial=true};
    }
    spec[0].format=RTCP_VEHICLE_FLOAT; spec[0].has_range=true;
    spec[0].minimum=-2; spec[0].maximum=10; spec[0].step=0.25;
    spec[0].initial=(rtcp_vehicle_value_t){.format=RTCP_VEHICLE_FLOAT,.as.real=-1.25};
    spec[1].format=RTCP_VEHICLE_UINT64;
    spec[1].initial=(rtcp_vehicle_value_t){.format=RTCP_VEHICLE_UINT64,.as.integer=INT64_C(9007199254740993)};
    spec[2].format=RTCP_VEHICLE_STRING; spec[2].maximum_length=256;
    spec[2].initial=(rtcp_vehicle_value_t){.format=RTCP_VEHICLE_STRING};
    const uint8_t unicode[]={0x41,0,0xe8,0xbd,0xa6,0xf0,0x9f,0x9a,0x97};
    memcpy(spec[2].initial.as.blob.bytes,unicode,sizeof(unicode)); spec[2].initial.as.blob.length=sizeof(unicode);
    spec[3].format=RTCP_VEHICLE_DATA; spec[3].maximum_length=256;
    spec[3].initial=(rtcp_vehicle_value_t){.format=RTCP_VEHICLE_DATA};
    spec[3].initial.as.blob.bytes[0]=0xff; spec[3].initial.as.blob.length=1;
    rtcp_vehicle_config_t cfg={.struct_size=sizeof(cfg),.plugin_name="\xe8\xbd\xa6\xf0\x9f\x9a\x97",.plugin_id=9,
        .characteristics=spec,.count=4};
    rtcp_vehicle_t *vehicle;
    assert(rtcp_vehicle_create(&cfg,&vehicle) == RTCP_OK);
    uint8_t bytes[AIRPLAY_INFO_PLIST_MAX_BYTES]; size_t length=0;
    airplay_info_plist_config_t info=k_config; info.vehicle=vehicle;
    assert(airplay_info_plist_build(&info,bytes,sizeof(bytes),&length));
    assert(airplay_info_plist_matches_vehicle(bytes,length,vehicle));
    if (match_file) {
        FILE *input=fopen(match_file,"rb"); assert(input);
        uint8_t supplied[AIRPLAY_INFO_PLIST_MAX_BYTES+1];
        size_t size=fread(supplied,1,sizeof(supplied),input); assert(!ferror(input)); fclose(input);
        assert(airplay_info_plist_matches_vehicle(supplied,size,vehicle) == match_expected);
    }
    if (dump) { printf("VEHICLE_INFO "); for (size_t i=0;i<length;++i) printf("%02x",bytes[i]); puts(""); }
    rtcp_vehicle_value_t changed={.format=RTCP_VEHICLE_UINT64,.as.integer=INT64_C(9007199254740994)};
    assert(rtcp_vehicle_set(vehicle,4,&changed) == RTCP_OK);
    assert(!airplay_info_plist_matches_vehicle(bytes,length,vehicle));
    assert(airplay_info_plist_build(&info,bytes,sizeof(bytes),&length));
    assert(airplay_info_plist_matches_vehicle(bytes,length,vehicle));
    /* Every truncation and output shortage must fail without an apparent document. */
    for (size_t n=0;n<length;++n) assert(!airplay_info_plist_matches_vehicle(bytes,n,vehicle));
    assert(!airplay_info_plist_build(&info,bytes,64,&length) && length == 0);
    info.vehicle=NULL;
    assert(airplay_info_plist_build(&info,bytes,sizeof(bytes),&length));
    assert(!airplay_info_plist_matches_vehicle(bytes,length,vehicle));
    /* Audio plus a small catalogue fits the existing bounded session buffer. */
    rtcp_vehicle_destroy(vehicle); cfg.count=1;
    assert(rtcp_vehicle_create(&cfg,&vehicle) == RTCP_OK);
    info.vehicle=vehicle; info.audio_output=true;
    assert(airplay_info_plist_build(&info,bytes,sizeof(bytes),&length));
    assert(airplay_info_plist_matches_vehicle(bytes,length,vehicle));
    rtcp_vehicle_destroy(vehicle);
    spec[0].format=RTCP_VEHICLE_INT32; spec[0].has_range=false;
    spec[0].initial=(rtcp_vehicle_value_t){.format=RTCP_VEHICLE_INT32,.as.integer=-7};
    assert(rtcp_vehicle_create(&cfg,&vehicle) == RTCP_OK); info.vehicle=vehicle;
    assert(airplay_info_plist_build(&info,bytes,sizeof(bytes),&length));
    assert(airplay_info_plist_matches_vehicle(bytes,length,vehicle));
    rtcp_vehicle_destroy(vehicle);
    rtcp_vehicle_characteristic_t large[RTCP_VEHICLE_MAX_CHARACTERISTICS];
    for (unsigned i=0;i<RTCP_VEHICLE_MAX_CHARACTERISTICS;++i) {
        large[i]=spec[3]; large[i].iid=3+i; large[i].initial.as.blob.length=256;
        memset(large[i].initial.as.blob.bytes,0xff,256);
    }
    cfg.characteristics=large; cfg.count=RTCP_VEHICLE_MAX_CHARACTERISTICS;
    assert(rtcp_vehicle_create(&cfg,&vehicle) == RTCP_OK); info.vehicle=vehicle;
    assert(!airplay_info_plist_build(&info,bytes,sizeof(bytes),&length) && !length);
    rtcp_vehicle_destroy(vehicle);
}

static void test_ui_info(bool dump, const char *file, bool expected)
{
    /* Synthetic single-Main-Display inventory; no official asset/renderer. */
    const char *uuid="b7e6c5a0-1111-4000-8000-000000000001";
    rtcp_ui_sync_config_t cfg={.struct_size=sizeof(cfg),.display_uuid=uuid};
    rtcp_ui_sync_t *ui; assert(rtcp_ui_sync_create(&cfg,&ui)==RTCP_OK);
    airplay_info_plist_config_t info=k_config; info.ui_sync=ui;
    uint8_t bytes[AIRPLAY_INFO_PLIST_MAX_BYTES]; size_t length=123;
    assert(!airplay_info_plist_build(&info,bytes,sizeof(bytes),&length) && !length);
    const char *palettes[]={"synthetic-light"};
    rtcp_ui_sync_inventory_layout_t layout={.id="synthetic-layout",.palettes=palettes,.palette_count=1};
    rtcp_ui_sync_inventory_t inv={.struct_size=sizeof(inv),.theme_id="synthetic:1",
        .panel_uid="synthetic-main-panel",.display_uuid=uuid,.width_pixels=1280,.height_pixels=480,
        .width_physical_mm=300,.height_physical_mm=120,.layouts=&layout,.layout_count=1};
    assert(rtcp_ui_sync_bind_inventory(ui,&inv)==RTCP_OK);
    assert(airplay_info_plist_build(&info,bytes,sizeof(bytes),&length));
    assert(airplay_info_plist_matches_ui_sync(bytes,length,ui));
    assert(!airplay_info_plist_matches_ui_sync(NULL,length,ui));
    assert(!airplay_info_plist_matches_ui_sync(bytes,length,NULL));
    assert(!airplay_info_plist_matches_ui_sync(bytes,sizeof(bytes)+1,ui));
    for (size_t i=0;i<length;++i) assert(!airplay_info_plist_matches_ui_sync(bytes,i,ui));
    size_t full=length;
    if (file) {
        FILE *input=fopen(file,"rb"); assert(input);
        uint8_t supplied[AIRPLAY_INFO_PLIST_MAX_BYTES+1];
        size_t n=fread(supplied,1,sizeof(supplied),input); assert(!ferror(input)); fclose(input);
        assert(airplay_info_plist_matches_ui_sync(supplied,n,ui)==expected);
    }
    if (dump) {
        printf("UI_INFO "); for (size_t i=0;i<length;++i) printf("%02x",bytes[i]); puts("");
        for (unsigned mask=0;mask<4;++mask) {
            uint8_t setup[512]; size_t n;
            assert(airplay_bplist_build_initial_setup_response_features(49152,49153,(mask&1)!=0,(mask&2)!=0,setup,sizeof(setup),&n)==AIRPLAY_BPLIST_OK);
            printf("UI_FEATURES %u ",mask); for (size_t i=0;i<n;++i) printf("%02x",setup[i]); puts("");
        }
    }
    for (size_t i=0;i<full;++i) assert(!airplay_info_plist_build(&info,bytes,i,&length) && !length);
    info.width_pixels++; assert(!airplay_info_plist_build(&info,bytes,sizeof(bytes),&length) && !length); info.width_pixels--;
    info.height_pixels++; assert(!airplay_info_plist_build(&info,bytes,sizeof(bytes),&length) && !length); info.height_pixels--;
    info.width_physical_mm++; assert(!airplay_info_plist_build(&info,bytes,sizeof(bytes),&length) && !length); info.width_physical_mm--;
    info.height_physical_mm++; assert(!airplay_info_plist_build(&info,bytes,sizeof(bytes),&length) && !length); info.height_physical_mm--;
    rtcp_display_profile_t profile={.struct_size=sizeof(profile),.uuid="11111111-2222-4333-8444-555555555555"};
    info.main_display_profile=&profile;
    assert(!airplay_info_plist_build(&info,bytes,sizeof(bytes),&length) && !length);
    info.main_display_profile=NULL;
    /* Default physical geometry must also agree with the installed display. */
    info.width_physical_mm=0;
    assert(!airplay_info_plist_build(&info,bytes,sizeof(bytes),&length) && !length);
    info.width_physical_mm=300;
    info.audio_output=true; info.audio_input=true;
    rtcp_vehicle_characteristic_t c={.accessory_iid=1,.accessory_type=100,.service_iid=2,
        .service_type=200,.iid=3,.type=1003,.format=RTCP_VEHICLE_UINT32,.owner=RTCP_VEHICLE_CAR};
    rtcp_vehicle_config_t vc={.struct_size=sizeof(vc),.plugin_name="synthetic",.characteristics=&c,.count=1};
    rtcp_vehicle_t *vehicle; assert(rtcp_vehicle_create(&vc,&vehicle)==RTCP_OK); info.vehicle=vehicle;
    assert(airplay_info_plist_build(&info,bytes,sizeof(bytes),&length));
    assert(airplay_info_plist_matches_ui_sync(bytes,length,ui) && airplay_info_plist_matches_vehicle(bytes,length,vehicle));
    rtcp_vehicle_destroy(vehicle); info.vehicle=NULL;
    assert(rtcp_ui_sync_bind_inventory(ui,NULL)==RTCP_OK);
    assert(!airplay_info_plist_matches_ui_sync(bytes,length,ui));
    rtcp_ui_sync_destroy(ui);
    puts("UI /info: bound actual geometry, one Main panel, complete output bounds and independent validation passed");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1],"--ui-wire")) { test_ui_info(true,NULL,false); return 0; }
    if (argc == 4 && !strcmp(argv[1],"--ui-match")) { test_ui_info(false,argv[2],!strcmp(argv[3],"yes")); return 0; }
    test_ui_info(false,NULL,false);
    if (argc == 4 && !strcmp(argv[1],"--match")) { match_file=argv[2]; match_expected=!strcmp(argv[3],"yes"); }
    test_vehicle_info(argc == 2 && !strcmp(argv[1],"--wire"));
    uint8_t plist[AIRPLAY_INFO_PLIST_MAX_BYTES];
    size_t plist_len = 0U;
    assert(airplay_info_plist_build(&k_config, plist, sizeof(plist), &plist_len));
    assert(plist_len > 0U && plist_len <= sizeof(plist));
    assert(memcmp(plist, "bplist00", 8U) == 0);

    airplay_bplist_doc_t doc;
    assert(airplay_bplist_open(plist, plist_len, &doc) == AIRPLAY_BPLIST_OK);

    airplay_bplist_object_t root;
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);

    expect_string(&doc, root, "deviceID", "00:11:22:33:44:55");
    expect_string(&doc, root, "name", "Demo CarPlay");
    expect_string(&doc, root, "model", "D213");
    expect_string(&doc, root, "manufacturer", "ArtInChip");
    expect_string(&doc, root, "sourceVersion", AIRPLAY_INFO_DEFAULT_SOURCE_VERSION);
    expect_uint(&doc, root, "features", AIRPLAY_INFO_DEFAULT_FEATURES);
    expect_uint(&doc, root, "statusFlags", AIRPLAY_INFO_DEFAULT_STATUS_FLAGS);

    airplay_bplist_object_t value;
    bool boolean = false;
    assert(airplay_bplist_dict_get(&doc, root, "rightHandDrive", &value) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_bool(&doc, value, &boolean) == AIRPLAY_BPLIST_OK);
    assert(boolean);

    uint16_t width = 0U;
    uint16_t height = 0U;
    assert(airplay_bplist_get_main_display_size(&doc, &width, &height) ==
           AIRPLAY_BPLIST_OK);
    assert(width == 1280U && height == 480U);

    airplay_bplist_object_t displays;
    airplay_bplist_object_t display;
    airplay_bplist_object_t hid_devices;
    airplay_bplist_object_t hid;
    airplay_bplist_type_t type;
    uint64_t count = 0U;
    assert(airplay_bplist_dict_get(&doc, root, "displays", &displays) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, displays, &type, &count) ==
           AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_ARRAY && count == 1U);
    assert(airplay_bplist_array_get(&doc, displays, 0U, &display) ==
           AIRPLAY_BPLIST_OK);
    expect_uint(&doc, display, "type", 110U);
    expect_uint(&doc, display, "maxFPS", 30U);
    expect_uint(&doc, display, "widthPixels", 1280U);
    expect_uint(&doc, display, "heightPixels", 480U);
    expect_uint(&doc, display, "widthPhysical", 300U);
    expect_uint(&doc, display, "heightPhysical", 120U);
    expect_uint(&doc, display, "features", 0x08U);
    expect_uint(&doc, display, "primaryInputDevice", 1U);
    expect_string(&doc, display, "uuid",
                  "b7e6c5a0-1111-4000-8000-000000000001");

    airplay_bplist_object_t view_areas;
    airplay_bplist_object_t view_area;
    airplay_bplist_object_t safe_area;
    assert(airplay_bplist_dict_get(&doc, display, "viewAreas", &view_areas) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_array_get(&doc, view_areas, 0U, &view_area) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, view_area, "safeArea", &safe_area) ==
           AIRPLAY_BPLIST_OK);
    expect_uint(&doc, safe_area, "widthPixels", 1280U);
    assert(airplay_bplist_dict_get(&doc, safe_area, "drawUIOutsideSafeArea",
                                   &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_bool(&doc, value, &boolean) == AIRPLAY_BPLIST_OK);
    assert(boolean);

    assert(airplay_bplist_dict_get(&doc, root, "hidDevices", &hid_devices) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, hid_devices, &type, &count) ==
           AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_ARRAY && count == 1U);
    assert(airplay_bplist_array_get(&doc, hid_devices, 0U, &hid) ==
           AIRPLAY_BPLIST_OK);
    expect_uint(&doc, hid, "hidProductID", 1U);
    expect_uint(&doc, hid, "hidVendorID", 2U);
    expect_string(&doc, hid, "uuid", "2a2a2a2a");
    expect_string(&doc, hid, "displayUUID",
                  "b7e6c5a0-1111-4000-8000-000000000001");

    const uint8_t *descriptor = NULL;
    size_t descriptor_len = 0U;
    assert(airplay_bplist_dict_get(&doc, hid, "hidDescriptor", &value) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_data(&doc, value, &descriptor, &descriptor_len) ==
           AIRPLAY_BPLIST_OK);
    uint8_t expected_descriptor[AIRPLAY_HID_TOUCH_DESCRIPTOR_BYTES];
    size_t expected_len = 0U;
    assert(airplay_hid_touch_descriptor_build(1280U, 480U, expected_descriptor,
                                              sizeof(expected_descriptor),
                                              &expected_len));
    assert(descriptor_len == expected_len);
    assert(memcmp(descriptor, expected_descriptor, expected_len) == 0);

    /* Optional fields fall back to defaults; an absent BT address is empty. */
    airplay_info_plist_config_t minimal = {
        .device_id = "AA:BB:CC:DD:EE:FF",
        .width_pixels = 400,
        .height_pixels = 240,
    };
    assert(airplay_info_plist_build(&minimal, plist, sizeof(plist), &plist_len));
    assert(airplay_bplist_open(plist, plist_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    expect_string(&doc, root, "name", "rt-carplay");
    expect_string(&doc, root, "model", AIRPLAY_INFO_DEFAULT_MODEL);
    expect_uint(&doc, root, "features", AIRPLAY_INFO_DEFAULT_FEATURES);
    assert(airplay_bplist_dict_get(&doc, root, "bluetoothIDs", &value) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, value, &type, &count) ==
           AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_ARRAY && count == 0U);

    /* Non-ASCII names are encoded as UTF-16BE. */
    airplay_info_plist_config_t unicode = minimal;
    unicode.name = "CAR \xE6\xB5\x8B\xE8\xAF\x95"; /* "CAR 测试" */
    assert(airplay_info_plist_build(&unicode, plist, sizeof(plist), &plist_len));
    assert(airplay_bplist_open(plist, plist_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "name", &value) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, value, &type, &count) ==
           AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_UNICODE_STRING);
    assert(count == 6U); /* "CAR " + two UTF-16 code units */

    /* Audio advertisement: the feature bits change and both capability
     * arrays are present, PCM-only (no Opus/AAC). */
    airplay_info_plist_config_t audio = k_config;
    audio.audio_output = true;
    audio.audio_sample_rate = 48000;
    assert(airplay_info_plist_build(&audio, plist, sizeof(plist), &plist_len));
    assert(airplay_bplist_open(plist, plist_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    expect_uint(&doc, root, "features", AIRPLAY_INFO_DEFAULT_FEATURES_AUDIO);

    airplay_bplist_object_t object;
    airplay_bplist_type_t object_type;
    uint64_t item_count = 0U;
    assert(airplay_bplist_dict_get(&doc, root, "audioFormats", &object) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, object, &object_type, &item_count) ==
           AIRPLAY_BPLIST_OK);
    assert(object_type == AIRPLAY_BPLIST_ARRAY && item_count == 8U);
    airplay_bplist_object_t entry;
    assert(airplay_bplist_array_get(&doc, object, 0U, &entry) ==
           AIRPLAY_BPLIST_OK);
    expect_uint(&doc, entry, "type", 100U);
    expect_string(&doc, entry, "audioType", "compatibility");
    expect_uint(&doc, entry, "audioOutputFormats", 0x3fcULL | 0xc000ULL);
    assert(airplay_bplist_array_get(&doc, object, 5U, &entry) ==
           AIRPLAY_BPLIST_OK);
    expect_string(&doc, entry, "audioType", "telephony");
    expect_uint(&doc, entry, "audioOutputFormats", 0x154ULL | 0x4000ULL);

    assert(airplay_bplist_dict_get(&doc, root, "audioLatencies", &object) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, object, &object_type, &item_count) ==
           AIRPLAY_BPLIST_OK);
    assert(object_type == AIRPLAY_BPLIST_ARRAY && item_count == 9U);
    assert(airplay_bplist_array_get(&doc, object, 8U, &entry) ==
           AIRPLAY_BPLIST_OK);
    expect_uint(&doc, entry, "type", 102U);
    expect_string(&doc, entry, "audioType", "media");
    expect_uint(&doc, entry, "outputLatencyMicros", 0U);

#ifdef RTCP_AUDIO_CODECS
    audio.audio_compressed = true;
    assert(airplay_info_plist_build(&audio, plist, sizeof(plist), &plist_len));
    assert(airplay_bplist_open(plist, plist_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    expect_uint(&doc, root, "features", AIRPLAY_INFO_DEFAULT_FEATURES_AUDIO | AIRPLAY_INFO_AAC_LC_FEATURE);
    assert(airplay_bplist_dict_get(&doc, root, "audioFormats", &object) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_array_get(&doc, object, 8, &entry) == AIRPLAY_BPLIST_OK);
    expect_uint(&doc, entry, "type", 102);
    expect_string(&doc, entry, "audioType", "media");
    expect_uint(&doc, entry, "audioOutputFormats", 0x800000);
    audio.audio_compressed = false;
#endif
    audio.audio_sample_rate = 44100;
    assert(airplay_info_plist_build(&audio, plist, sizeof(plist), &plist_len));
    assert(airplay_bplist_open(plist, plist_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "audioFormats", &object) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_array_get(&doc, object, 0U, &entry) ==
           AIRPLAY_BPLIST_OK);
    expect_uint(&doc, entry, "audioOutputFormats", 0x3fcULL | 0xc00ULL);

    /* A receiver with both decoders must let the controller select its media
     * rate, rather than constrain negotiation to the old local 44.1 kHz default. */
    audio.audio_sample_rate = 0;
    audio.audio_compressed = true;
    assert(airplay_info_plist_build(&audio, plist, sizeof(plist), &plist_len));
    assert(airplay_bplist_open(plist, plist_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "audioFormats", &object) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_array_get(&doc, object, 4, &entry) == AIRPLAY_BPLIST_OK);
    expect_string(&doc, entry, "audioType", "media");
    expect_uint(&doc, entry, "audioOutputFormats", 0xcffc);
    assert(airplay_bplist_get_type(&doc, object, &object_type, &item_count) == AIRPLAY_BPLIST_OK);
#ifdef RTCP_AUDIO_CODECS
    assert(item_count == 9);
    assert(airplay_bplist_array_get(&doc, object, 8, &entry) == AIRPLAY_BPLIST_OK);
    expect_uint(&doc, entry, "type", 102);
    expect_string(&doc, entry, "audioType", "media");
    expect_uint(&doc, entry, "audioOutputFormats", 0xc00000);
#else
    assert(item_count == 8);
#endif
    uint64_t offered_features = airplay_info_plist_features(&audio);
    assert(offered_features & (UINT64_C(1) << 18));
    assert(!!(offered_features & AIRPLAY_INFO_AAC_LC_FEATURE) == (item_count == 9));
    for (uint64_t i = 0; i < item_count; ++i) {
        uint64_t bits;
        assert(airplay_bplist_array_get(&doc, object, i, &entry) == AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_dict_get(&doc, entry, "audioOutputFormats", &value) == AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_get_uint(&doc, value, &bits) == AIRPLAY_BPLIST_OK);
        assert((bits & ~rtcp_audio_output_formats()) == 0);
        /* Output-only is represented by an omitted input field (DiPlay and
         * catplay), rather than an explicit zero-capability microphone.
         */
        assert(airplay_bplist_dict_get(&doc, entry, "audioInputFormats", &value) == AIRPLAY_BPLIST_NOT_FOUND);
    }
    /* Wire semantics of the working DiPlay initial-mode declaration. This is
     * distinct from an accessory taking audio at runtime over /command.
     */
    assert(airplay_bplist_dict_get(&doc, root, "modes", &object) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, object, "resources", &object) == AIRPLAY_BPLIST_OK);
    for (uint64_t i = 0; i < 2; ++i) {
        assert(airplay_bplist_array_get(&doc, object, i, &entry) == AIRPLAY_BPLIST_OK);
        expect_uint(&doc, entry, "resourceID", i+1);
        expect_uint(&doc, entry, "transferType", 1);
        expect_uint(&doc, entry, "transferPriority", 100);
        expect_uint(&doc, entry, "takeConstraint", 100);
        expect_uint(&doc, entry, "borrowConstraint", 100);
    }

    /* Optional profile is coherent on the wire: custom UUID applies to HID
     * and display; view/safe geometry is bounded in negotiated pixels. */
    rtcp_display_profile_t profile = {
        .struct_size = sizeof(profile),
        .uuid = "01234567-89ab-cdef-0123-456789abcdef",
        .view_area = {20, 10, 1240, 460},
        .safe_area = {30, 20, 1220, 440},
        .ui_night = true, .map_night = true, .appearance_setting = 2
    };
    airplay_info_plist_config_t custom = k_config;
    custom.main_display_profile = &profile;
    assert(airplay_info_plist_build(&custom, plist, sizeof(plist), &plist_len));
    assert(airplay_bplist_open(plist, plist_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "displays", &object) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, object, &object_type, &item_count) == AIRPLAY_BPLIST_OK);
    assert(item_count == 1); /* No implicit second display. */
    assert(airplay_bplist_array_get(&doc, object, 0, &entry) == AIRPLAY_BPLIST_OK);
    expect_string(&doc, entry, "uuid", profile.uuid);
    expect_uint(&doc, entry, "type", 110);
    expect_uint(&doc, entry, "uiAppearanceMode", 1);
    expect_uint(&doc, entry, "mapAppearanceSetting", 2);
    assert(airplay_bplist_dict_get(&doc, entry, "viewAreas", &object) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_array_get(&doc, object, 0, &entry) == AIRPLAY_BPLIST_OK);
    expect_uint(&doc, entry, "originXPixels", 20);
    expect_uint(&doc, entry, "widthPixels", 1240);
    assert(airplay_bplist_dict_get(&doc, entry, "safeArea", &entry) == AIRPLAY_BPLIST_OK);
    expect_uint(&doc, entry, "originXPixels", 30);
    expect_uint(&doc, entry, "heightPixels", 440);
    assert(airplay_bplist_dict_get(&doc, root, "hidDevices", &object) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_array_get(&doc, object, 0, &entry) == AIRPLAY_BPLIST_OK);
    expect_string(&doc, entry, "displayUUID", profile.uuid);
    assert(airplay_bplist_dict_get(&doc, root, "hevcInfo", &object) == AIRPLAY_BPLIST_NOT_FOUND);
    assert(airplay_bplist_dict_get(&doc, root, "vehicleStateProtocolInfo", &object) == AIRPLAY_BPLIST_NOT_FOUND);
    assert(airplay_bplist_dict_get(&doc, root, "uiSyncInfo", &object) == AIRPLAY_BPLIST_NOT_FOUND);
    profile.safe_area.x = 0;
    assert(!airplay_info_plist_build(&custom, plist, sizeof(plist), &plist_len));
    assert(plist_len == 0);

    /* Bounds and argument validation. */
    size_t unused = 0U;
    uint8_t tiny[64];
    assert(!airplay_info_plist_build(&k_config, tiny, sizeof(tiny), &unused));
    assert(unused == 0U);
    assert(!airplay_info_plist_build(NULL, plist, sizeof(plist), &unused));
    airplay_info_plist_config_t invalid = k_config;
    invalid.device_id = NULL;
    assert(!airplay_info_plist_build(&invalid, plist, sizeof(plist), &unused));
    invalid = k_config;
    invalid.width_pixels = 0U;
    assert(!airplay_info_plist_build(&invalid, plist, sizeof(plist), &unused));
    assert(!airplay_info_plist_build(&k_config, NULL, sizeof(plist), &unused));
    assert(!airplay_info_plist_build(&k_config, plist, sizeof(plist), NULL));

    puts("airplay /info plist host test passed");
    return 0;
}
