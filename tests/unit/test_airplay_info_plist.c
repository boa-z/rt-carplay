/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_info_plist.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "airplay_bplist.h"
#include "airplay_hid_descriptor.h"

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

int main(void)
{
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
    expect_uint(&doc, entry, "outputLatencyMicros", 0U);

    audio.audio_sample_rate = 44100;
    assert(airplay_info_plist_build(&audio, plist, sizeof(plist), &plist_len));
    assert(airplay_bplist_open(plist, plist_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "audioFormats", &object) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_array_get(&doc, object, 0U, &entry) ==
           AIRPLAY_BPLIST_OK);
    expect_uint(&doc, entry, "audioOutputFormats", 0x3fcULL | 0xc00ULL);

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
