/* SPDX-License-Identifier: GPL-3.0-or-later; binary plist subset follows DiPlay */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_BPLIST_MAX_OBJECTS 4096U

typedef struct {
    const uint8_t *bytes;
    size_t length;
    size_t trailer_offset;
    size_t offset_table_offset;
    uint64_t object_count;
    uint64_t top_object;
    uint8_t offset_size;
    uint8_t reference_size;
    bool valid;
} airplay_bplist_doc_t;

typedef struct {
    uint64_t id;
} airplay_bplist_object_t;

typedef enum {
    AIRPLAY_BPLIST_OK = 0,
    AIRPLAY_BPLIST_INVALID_ARGUMENT,
    AIRPLAY_BPLIST_INVALID_DOCUMENT,
    AIRPLAY_BPLIST_INVALID_OBJECT,
    AIRPLAY_BPLIST_TOO_LARGE,
    AIRPLAY_BPLIST_NOT_FOUND,
    AIRPLAY_BPLIST_WRONG_TYPE,
    AIRPLAY_BPLIST_OUTPUT_TOO_SMALL,
} airplay_bplist_result_t;

typedef enum {
    AIRPLAY_BPLIST_NULL = 0,
    AIRPLAY_BPLIST_BOOL,
    AIRPLAY_BPLIST_INTEGER,
    AIRPLAY_BPLIST_REAL,
    AIRPLAY_BPLIST_DATA,
    AIRPLAY_BPLIST_ASCII_STRING,
    AIRPLAY_BPLIST_UNICODE_STRING,
    AIRPLAY_BPLIST_ARRAY,
    AIRPLAY_BPLIST_DICTIONARY,
    AIRPLAY_BPLIST_OTHER,
} airplay_bplist_type_t;

airplay_bplist_result_t airplay_bplist_open(const uint8_t *bytes,
                                            size_t length,
                                            airplay_bplist_doc_t *doc);
airplay_bplist_result_t airplay_bplist_root(const airplay_bplist_doc_t *doc,
                                            airplay_bplist_object_t *object);
airplay_bplist_result_t airplay_bplist_get_type(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t object,
    airplay_bplist_type_t *type,
    uint64_t *item_count);
airplay_bplist_result_t airplay_bplist_array_get(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t array,
    uint64_t index,
    airplay_bplist_object_t *value);
airplay_bplist_result_t airplay_bplist_dict_get(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t dictionary,
    const char *key,
    airplay_bplist_object_t *value);
/* Read widthPixels/heightPixels from the first (main) entry in an AirPlay /info displays array. */
airplay_bplist_result_t airplay_bplist_get_main_display_size(
    const airplay_bplist_doc_t *doc,
    uint16_t *width_pixels,
    uint16_t *height_pixels);
airplay_bplist_result_t airplay_bplist_get_uint(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t object,
    uint64_t *value);
airplay_bplist_result_t airplay_bplist_get_bool(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t object,
    bool *value);
airplay_bplist_result_t airplay_bplist_get_string(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t object,
    char *output,
    size_t output_capacity,
    size_t *output_len);
airplay_bplist_result_t airplay_bplist_get_data(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t object,
    const uint8_t **bytes,
    size_t *length);

/* Build bounded AirPlay SETUP binary-plist responses supported by this relay. */
airplay_bplist_result_t airplay_bplist_build_initial_setup_response(
    uint16_t timing_port,
    uint16_t event_port,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len);
airplay_bplist_result_t airplay_bplist_build_screen_setup_response(
    uint64_t stream_type,
    uint16_t data_port,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len);
airplay_bplist_result_t airplay_bplist_build_empty_stream_setup_response(
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len);
