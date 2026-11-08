/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_bplist.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Python plistlib binary encoding of:
 * {"streams": [{"streamConnectionID": 12345, "type": 110}]}
 */
static const uint8_t k_setup_plist[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30,
    0xd1, 0x01, 0x02,
    0x57, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x73,
    0xa1, 0x03,
    0xd2, 0x04, 0x05, 0x06, 0x07,
    0x5f, 0x10, 0x12, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d,
    0x43, 0x6f, 0x6e, 0x6e, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x49, 0x44,
    0x54, 0x74, 0x79, 0x70, 0x65,
    0x11, 0x30, 0x39,
    0x10, 0x6e,
    0x08, 0x0b, 0x13, 0x15, 0x1a, 0x2f, 0x34, 0x37,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x39,
};

/* Python plistlib binary encoding of a /info main display at 1920x1080. */
static const uint8_t k_display_info_plist[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30,
    0xd1, 0x01, 0x02,
    0x58, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79, 0x73,
    0xa1, 0x03,
    0xd2, 0x04, 0x05, 0x06, 0x07,
    0x5b, 0x77, 0x69, 0x64, 0x74, 0x68, 0x50, 0x69, 0x78, 0x65, 0x6c, 0x73,
    0x5c, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x50, 0x69, 0x78, 0x65, 0x6c, 0x73,
    0x11, 0x07, 0x80,
    0x11, 0x04, 0x38,
    0x08, 0x0b, 0x14, 0x16, 0x1b, 0x27, 0x34, 0x37,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3a,
};

/* Same structure with widthPixels=65536, which cannot fit the HID report coordinates. */
static const uint8_t k_oversize_display_info_plist[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30,
    0xd1, 0x01, 0x02,
    0x58, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79, 0x73,
    0xa1, 0x03,
    0xd2, 0x04, 0x05, 0x06, 0x07,
    0x5b, 0x77, 0x69, 0x64, 0x74, 0x68, 0x50, 0x69, 0x78, 0x65, 0x6c, 0x73,
    0x5c, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x50, 0x69, 0x78, 0x65, 0x6c, 0x73,
    0x12, 0x00, 0x01, 0x00, 0x00,
    0x11, 0x04, 0x38,
    0x08, 0x0b, 0x14, 0x16, 0x1b, 0x27, 0x34, 0x39,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c,
};

static void test_setup_stream_lookup(void)
{
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    airplay_bplist_object_t streams;
    airplay_bplist_object_t stream;
    airplay_bplist_object_t value;
    airplay_bplist_type_t type;
    uint64_t count = 0U;
    uint64_t number = 0U;
    char name[32];
    size_t name_len = 0U;

    assert(airplay_bplist_open(k_setup_plist, sizeof(k_setup_plist), &doc) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, root, &type, &count) == AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_DICTIONARY && count == 1U);
    assert(airplay_bplist_dict_get(&doc, root, "streams", &streams) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, streams, &type, &count) == AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_ARRAY && count == 1U);
    assert(airplay_bplist_array_get(&doc, streams, 0U, &stream) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, stream, "streamConnectionID", &value) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(&doc, value, &number) == AIRPLAY_BPLIST_OK);
    assert(number == 12345U);
    assert(airplay_bplist_dict_get(&doc, stream, "type", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, value, &type, NULL) == AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_INTEGER);
    assert(airplay_bplist_get_uint(&doc, value, &number) == AIRPLAY_BPLIST_OK);
    assert(number == 110U);

    assert(airplay_bplist_get_string(&doc, (airplay_bplist_object_t){.id = 1U},
                                     name, sizeof(name), &name_len) == AIRPLAY_BPLIST_OK);
    assert(name_len == 7U && strcmp(name, "streams") == 0);
    assert(airplay_bplist_get_type(&doc, (airplay_bplist_object_t){.id = 1U},
                                   &type, NULL) == AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_ASCII_STRING);
    assert(airplay_bplist_dict_get(&doc, root, "missing", &value) ==
           AIRPLAY_BPLIST_NOT_FOUND);
    assert(airplay_bplist_array_get(&doc, streams, 1U, &value) ==
           AIRPLAY_BPLIST_NOT_FOUND);
}

static void test_main_display_size(void)
{
    airplay_bplist_doc_t doc;
    uint16_t width = 0U;
    uint16_t height = 0U;

    assert(airplay_bplist_open(k_display_info_plist,
                               sizeof(k_display_info_plist), &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_main_display_size(&doc, &width, &height) == AIRPLAY_BPLIST_OK);
    assert(width == 1920U && height == 1080U);

    width = 99U;
    height = 88U;
    assert(airplay_bplist_open(k_setup_plist, sizeof(k_setup_plist), &doc) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_main_display_size(&doc, &width, &height) ==
           AIRPLAY_BPLIST_NOT_FOUND);
    assert(width == 0U && height == 0U);

    assert(airplay_bplist_open(k_oversize_display_info_plist,
                               sizeof(k_oversize_display_info_plist), &doc) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_main_display_size(&doc, &width, &height) ==
           AIRPLAY_BPLIST_TOO_LARGE);
    assert(width == 0U && height == 0U);
}

static void test_rejects_malformed_documents(void)
{
    airplay_bplist_doc_t doc;
    uint8_t malformed[sizeof(k_setup_plist)];

    assert(airplay_bplist_open(k_setup_plist, sizeof(k_setup_plist) - 1U, &doc) !=
           AIRPLAY_BPLIST_OK);
    memcpy(malformed, k_setup_plist, sizeof(malformed));
    malformed[0] = 'x';
    assert(airplay_bplist_open(malformed, sizeof(malformed), &doc) ==
           AIRPLAY_BPLIST_INVALID_DOCUMENT);

    memcpy(malformed, k_setup_plist, sizeof(malformed));
    malformed[20] = 0xffU; /* array element references an absent object */
    assert(airplay_bplist_open(malformed, sizeof(malformed), &doc) == AIRPLAY_BPLIST_OK);
    airplay_bplist_object_t streams = {.id = 2U};
    airplay_bplist_object_t value;
    assert(airplay_bplist_array_get(&doc, streams, 0U, &value) ==
           AIRPLAY_BPLIST_INVALID_OBJECT);

    memcpy(malformed, k_setup_plist, sizeof(malformed));
    malformed[57] = 0U; /* root object's offset points into the header */
    assert(airplay_bplist_open(malformed, sizeof(malformed), &doc) == AIRPLAY_BPLIST_OK);
    airplay_bplist_object_t root;
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    airplay_bplist_type_t type;
    assert(airplay_bplist_get_type(&doc, root, &type, NULL) ==
           AIRPLAY_BPLIST_INVALID_OBJECT);
}

static void test_builds_setup_responses(void)
{
    uint8_t output[256];
    size_t output_len = 0U;
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    airplay_bplist_object_t streams;
    airplay_bplist_object_t stream;
    airplay_bplist_object_t value;
    airplay_bplist_type_t type;
    uint64_t count = 0U;
    uint64_t number = 0U;

    assert(airplay_bplist_build_screen_setup_response(
               110U, 49152U, output, sizeof(output), &output_len) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_open(output, output_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "streams", &streams) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, streams, &type, &count) == AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_ARRAY && count == 1U);
    assert(airplay_bplist_array_get(&doc, streams, 0U, &stream) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, stream, "type", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(&doc, value, &number) == AIRPLAY_BPLIST_OK);
    assert(number == 110U);
    assert(airplay_bplist_dict_get(&doc, stream, "dataPort", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(&doc, value, &number) == AIRPLAY_BPLIST_OK);
    assert(number == 49152U);

    assert(airplay_bplist_build_empty_stream_setup_response(
               output, sizeof(output), &output_len) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_open(output, output_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "streams", &streams) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, streams, &type, &count) == AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_ARRAY && count == 0U);

    assert(airplay_bplist_build_initial_setup_response(
               49152U, 49153U, output, sizeof(output), &output_len) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_open(output, output_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "eventPort", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(&doc, value, &number) == AIRPLAY_BPLIST_OK);
    assert(number == 49153U);
    assert(airplay_bplist_dict_get(&doc, root, "timingPort", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(&doc, value, &number) == AIRPLAY_BPLIST_OK);
    assert(number == 49152U);
    assert(airplay_bplist_build_initial_setup_response(
               49152U, 49153U, output, 16U, &output_len) == AIRPLAY_BPLIST_OUTPUT_TOO_SMALL);
}

int main(void)
{
    test_setup_stream_lookup();
    test_main_display_size();
    test_rejects_malformed_documents();
    test_builds_setup_responses();
    puts("airplay binary plist host test passed");
    return 0;
}
