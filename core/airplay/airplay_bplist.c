/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bounded binary plist reader for the scalar, array, and dictionary subset
 * used by DiPlay's AirPlay /info and SETUP messages. It keeps views into the
 * caller's input buffer and does not build an unbounded heap object tree.
 */
#include "airplay_bplist.h"

#include <string.h>

#define BPLIST_HEADER_BYTES 8U
#define BPLIST_TRAILER_BYTES 32U
#define BPLIST_WRITER_MAX_OBJECTS 12U

typedef struct {
    uint8_t marker;
    uint8_t type;
    uint64_t count;
    size_t payload_offset;
    size_t payload_bytes;
} bplist_object_info_t;

static bool supported_width(uint8_t width)
{
    return width == 1U || width == 2U || width == 4U || width == 8U;
}

static bool read_be(const uint8_t *bytes, size_t available,
                    size_t offset, uint8_t width, uint64_t *value)
{
    if (bytes == NULL || value == NULL || !supported_width(width) ||
        offset > available || width > available - offset) return false;
    uint64_t result = 0U;
    for (uint8_t i = 0U; i < width; ++i) {
        result = (result << 8U) | bytes[offset + i];
    }
    *value = result;
    return true;
}

static airplay_bplist_result_t resolve_object(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t object,
    bplist_object_info_t *info)
{
    if (doc == NULL || info == NULL || !doc->valid ||
        object.id >= doc->object_count) {
        return AIRPLAY_BPLIST_INVALID_OBJECT;
    }
    uint64_t offset64 = 0U;
    if (!read_be(doc->bytes, doc->trailer_offset,
                 doc->offset_table_offset + (size_t)object.id * doc->offset_size,
                 doc->offset_size, &offset64) ||
        offset64 < BPLIST_HEADER_BYTES || offset64 >= doc->offset_table_offset) {
        return AIRPLAY_BPLIST_INVALID_OBJECT;
    }
    size_t offset = (size_t)offset64;
    uint8_t marker = doc->bytes[offset];
    uint8_t type = marker >> 4U;
    uint8_t nibble = marker & 0x0fU;
    size_t cursor = offset + 1U;
    uint64_t count = nibble;

    if (type == 0U) {
        if (nibble != 0x00U && nibble != 0x08U && nibble != 0x09U) {
            return AIRPLAY_BPLIST_INVALID_OBJECT;
        }
        info->marker = marker;
        info->type = nibble == 0x08U || nibble == 0x09U ? 1U : 0U;
        info->count = 0U;
        info->payload_offset = cursor;
        info->payload_bytes = 0U;
        return AIRPLAY_BPLIST_OK;
    }
    if (type == 0x01U || type == 0x02U) {
        if (nibble > 3U) return AIRPLAY_BPLIST_INVALID_OBJECT;
        size_t width = (size_t)1U << nibble;
        if (cursor > doc->offset_table_offset ||
            width > doc->offset_table_offset - cursor) {
            return AIRPLAY_BPLIST_INVALID_OBJECT;
        }
        info->marker = marker;
        info->type = type;
        info->count = 1U;
        info->payload_offset = cursor;
        info->payload_bytes = width;
        return AIRPLAY_BPLIST_OK;
    }

    if (nibble == 0x0fU) {
        if (cursor >= doc->offset_table_offset) return AIRPLAY_BPLIST_INVALID_OBJECT;
        uint8_t length_marker = doc->bytes[cursor++];
        if ((length_marker >> 4U) != 0x01U ||
            (length_marker & 0x0fU) > 3U) {
            return AIRPLAY_BPLIST_INVALID_OBJECT;
        }
        uint8_t int_width = (uint8_t)(1U << (length_marker & 0x0fU));
        if (!read_be(doc->bytes, doc->offset_table_offset, cursor,
                     int_width, &count)) {
            return AIRPLAY_BPLIST_INVALID_OBJECT;
        }
        cursor += int_width;
    }

    size_t payload_bytes = 0U;
    if (type == 0x04U || type == 0x05U) {
        if (count > SIZE_MAX) return AIRPLAY_BPLIST_TOO_LARGE;
        payload_bytes = (size_t)count;
    } else if (type == 0x06U) {
        if (count > SIZE_MAX / 2U) return AIRPLAY_BPLIST_TOO_LARGE;
        payload_bytes = (size_t)count * 2U;
    } else if (type == 0x0aU || type == 0x0cU) {
        if (count > SIZE_MAX / doc->reference_size) return AIRPLAY_BPLIST_TOO_LARGE;
        payload_bytes = (size_t)count * doc->reference_size;
    } else if (type == 0x0dU) {
        if (count > SIZE_MAX / (2U * doc->reference_size)) return AIRPLAY_BPLIST_TOO_LARGE;
        payload_bytes = (size_t)count * 2U * doc->reference_size;
    } else {
        return AIRPLAY_BPLIST_INVALID_OBJECT;
    }
    if (cursor > doc->offset_table_offset ||
        payload_bytes > doc->offset_table_offset - cursor) {
        return AIRPLAY_BPLIST_INVALID_OBJECT;
    }
    info->marker = marker;
    info->type = type;
    info->count = count;
    info->payload_offset = cursor;
    info->payload_bytes = payload_bytes;
    return AIRPLAY_BPLIST_OK;
}

static airplay_bplist_result_t read_reference(
    const airplay_bplist_doc_t *doc,
    size_t offset,
    airplay_bplist_object_t *object)
{
    uint64_t reference = 0U;
    if (object == NULL ||
        !read_be(doc->bytes, doc->offset_table_offset, offset,
                 doc->reference_size, &reference) ||
        reference >= doc->object_count) {
        return AIRPLAY_BPLIST_INVALID_OBJECT;
    }
    object->id = reference;
    return AIRPLAY_BPLIST_OK;
}

static bool key_equals(const airplay_bplist_doc_t *doc,
                       airplay_bplist_object_t key_object,
                       const char *key)
{
    bplist_object_info_t info;
    if (resolve_object(doc, key_object, &info) != AIRPLAY_BPLIST_OK ||
        (info.type != 0x05U && info.type != 0x06U)) return false;
    size_t key_len = strlen(key);
    if (info.count != key_len) return false;
    if (info.type == 0x05U) {
        return memcmp(doc->bytes + info.payload_offset, key, key_len) == 0;
    }
    for (size_t i = 0U; i < key_len; ++i) {
        if (doc->bytes[info.payload_offset + 2U * i] != 0U ||
            doc->bytes[info.payload_offset + 2U * i + 1U] != (uint8_t)key[i]) {
            return false;
        }
    }
    return true;
}

airplay_bplist_result_t airplay_bplist_open(const uint8_t *bytes,
                                            size_t length,
                                            airplay_bplist_doc_t *doc)
{
    static const uint8_t magic[BPLIST_HEADER_BYTES] = {
        'b', 'p', 'l', 'i', 's', 't', '0', '0',
    };
    if (doc == NULL) return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    memset(doc, 0, sizeof(*doc));
    if (bytes == NULL || length < BPLIST_HEADER_BYTES + BPLIST_TRAILER_BYTES ||
        memcmp(bytes, magic, sizeof(magic)) != 0) {
        return AIRPLAY_BPLIST_INVALID_DOCUMENT;
    }
    size_t trailer = length - BPLIST_TRAILER_BYTES;
    uint8_t offset_size = bytes[trailer + 6U];
    uint8_t reference_size = bytes[trailer + 7U];
    uint64_t object_count = 0U;
    uint64_t top_object = 0U;
    uint64_t table_offset = 0U;
    if (!supported_width(offset_size) || !supported_width(reference_size) ||
        !read_be(bytes, length, trailer + 8U, 8U, &object_count) ||
        !read_be(bytes, length, trailer + 16U, 8U, &top_object) ||
        !read_be(bytes, length, trailer + 24U, 8U, &table_offset)) {
        return AIRPLAY_BPLIST_INVALID_DOCUMENT;
    }
    if (object_count == 0U || object_count > AIRPLAY_BPLIST_MAX_OBJECTS ||
        top_object >= object_count || table_offset > trailer ||
        table_offset < BPLIST_HEADER_BYTES ||
        object_count > SIZE_MAX / offset_size ||
        (size_t)object_count * offset_size != trailer - (size_t)table_offset) {
        return AIRPLAY_BPLIST_TOO_LARGE;
    }
    doc->bytes = bytes;
    doc->length = length;
    doc->trailer_offset = trailer;
    doc->offset_table_offset = (size_t)table_offset;
    doc->object_count = object_count;
    doc->top_object = top_object;
    doc->offset_size = offset_size;
    doc->reference_size = reference_size;
    doc->valid = true;
    return AIRPLAY_BPLIST_OK;
}

airplay_bplist_result_t airplay_bplist_root(const airplay_bplist_doc_t *doc,
                                            airplay_bplist_object_t *object)
{
    if (doc == NULL || object == NULL || !doc->valid) {
        return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    }
    object->id = doc->top_object;
    return AIRPLAY_BPLIST_OK;
}

airplay_bplist_result_t airplay_bplist_get_type(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t object,
    airplay_bplist_type_t *type,
    uint64_t *item_count)
{
    if (type == NULL) return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    bplist_object_info_t info;
    airplay_bplist_result_t result = resolve_object(doc, object, &info);
    if (result != AIRPLAY_BPLIST_OK) return result;
    switch (info.type) {
    case 0x00U: *type = AIRPLAY_BPLIST_NULL; break;
    case 0x01U: *type = (info.marker >> 4U) == 0U
                              ? AIRPLAY_BPLIST_BOOL : AIRPLAY_BPLIST_INTEGER; break;
    case 0x02U: *type = AIRPLAY_BPLIST_REAL; break;
    case 0x04U: *type = AIRPLAY_BPLIST_DATA; break;
    case 0x05U: *type = AIRPLAY_BPLIST_ASCII_STRING; break;
    case 0x06U: *type = AIRPLAY_BPLIST_UNICODE_STRING; break;
    case 0x0aU: case 0x0cU: *type = AIRPLAY_BPLIST_ARRAY; break;
    case 0x0dU: *type = AIRPLAY_BPLIST_DICTIONARY; break;
    default: *type = AIRPLAY_BPLIST_OTHER; break;
    }
    if (item_count != NULL) *item_count = info.count;
    return AIRPLAY_BPLIST_OK;
}

airplay_bplist_result_t airplay_bplist_array_get(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t array,
    uint64_t index,
    airplay_bplist_object_t *value)
{
    if (value == NULL) return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    bplist_object_info_t info;
    airplay_bplist_result_t result = resolve_object(doc, array, &info);
    if (result != AIRPLAY_BPLIST_OK) return result;
    if (info.type != 0x0aU && info.type != 0x0cU) return AIRPLAY_BPLIST_WRONG_TYPE;
    if (index >= info.count) return AIRPLAY_BPLIST_NOT_FOUND;
    return read_reference(doc, info.payload_offset + (size_t)index * doc->reference_size,
                          value);
}

airplay_bplist_result_t airplay_bplist_dict_get(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t dictionary,
    const char *key,
    airplay_bplist_object_t *value)
{
    if (key == NULL || value == NULL) return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    bplist_object_info_t info;
    airplay_bplist_result_t result = resolve_object(doc, dictionary, &info);
    if (result != AIRPLAY_BPLIST_OK) return result;
    if (info.type != 0x0dU) return AIRPLAY_BPLIST_WRONG_TYPE;
    for (uint64_t i = 0U; i < info.count; ++i) {
        airplay_bplist_object_t key_object;
        result = read_reference(doc,
                                info.payload_offset + (size_t)i * doc->reference_size,
                                &key_object);
        if (result != AIRPLAY_BPLIST_OK) return result;
        if (key_equals(doc, key_object, key)) {
            return read_reference(
                doc, info.payload_offset +
                         ((size_t)info.count + (size_t)i) * doc->reference_size,
                value);
        }
    }
    return AIRPLAY_BPLIST_NOT_FOUND;
}

airplay_bplist_result_t airplay_bplist_get_main_display_size(
    const airplay_bplist_doc_t *doc,
    uint16_t *width_pixels,
    uint16_t *height_pixels)
{
    if (doc == NULL || width_pixels == NULL || height_pixels == NULL) {
        return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    }
    *width_pixels = 0U;
    *height_pixels = 0U;

    airplay_bplist_object_t root;
    airplay_bplist_object_t displays;
    airplay_bplist_object_t main_display;
    airplay_bplist_object_t width_object;
    airplay_bplist_object_t height_object;
    airplay_bplist_type_t type;
    uint64_t display_count = 0U;
    uint64_t width = 0U;
    uint64_t height = 0U;
    airplay_bplist_result_t result = airplay_bplist_root(doc, &root);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_dict_get(doc, root, "displays", &displays);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_get_type(doc, displays, &type, &display_count);
    if (result != AIRPLAY_BPLIST_OK) return result;
    if (type != AIRPLAY_BPLIST_ARRAY) return AIRPLAY_BPLIST_WRONG_TYPE;
    if (display_count == 0U) return AIRPLAY_BPLIST_NOT_FOUND;
    result = airplay_bplist_array_get(doc, displays, 0U, &main_display);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_dict_get(doc, main_display, "widthPixels", &width_object);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_dict_get(doc, main_display, "heightPixels", &height_object);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_get_uint(doc, width_object, &width);
    if (result != AIRPLAY_BPLIST_OK) return result;
    result = airplay_bplist_get_uint(doc, height_object, &height);
    if (result != AIRPLAY_BPLIST_OK) return result;
    if (width == 0U || height == 0U || width > UINT16_MAX || height > UINT16_MAX) {
        return AIRPLAY_BPLIST_TOO_LARGE;
    }
    *width_pixels = (uint16_t)width;
    *height_pixels = (uint16_t)height;
    return AIRPLAY_BPLIST_OK;
}

airplay_bplist_result_t airplay_bplist_get_uint(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t object,
    uint64_t *value)
{
    if (value == NULL) return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    bplist_object_info_t info;
    airplay_bplist_result_t result = resolve_object(doc, object, &info);
    if (result != AIRPLAY_BPLIST_OK) return result;
    if (info.type != 0x01U || info.marker >> 4U != 0x01U) {
        return AIRPLAY_BPLIST_WRONG_TYPE;
    }
    return read_be(doc->bytes, doc->offset_table_offset, info.payload_offset,
                   (uint8_t)info.payload_bytes, value)
               ? AIRPLAY_BPLIST_OK : AIRPLAY_BPLIST_INVALID_OBJECT;
}

airplay_bplist_result_t airplay_bplist_get_bool(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t object,
    bool *value)
{
    if (value == NULL) return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    bplist_object_info_t info;
    airplay_bplist_result_t result = resolve_object(doc, object, &info);
    if (result != AIRPLAY_BPLIST_OK) return result;
    if (info.type != 0U || (info.marker != 0x08U && info.marker != 0x09U)) {
        return AIRPLAY_BPLIST_WRONG_TYPE;
    }
    *value = info.marker == 0x09U;
    return AIRPLAY_BPLIST_OK;
}

airplay_bplist_result_t airplay_bplist_get_string(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t object,
    char *output,
    size_t output_capacity,
    size_t *output_len)
{
    if (output_len != NULL) *output_len = 0U;
    if (output == NULL || output_capacity == 0U || output_len == NULL) {
        return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    }
    bplist_object_info_t info;
    airplay_bplist_result_t result = resolve_object(doc, object, &info);
    if (result != AIRPLAY_BPLIST_OK) return result;
    if (info.type != 0x05U && info.type != 0x06U) return AIRPLAY_BPLIST_WRONG_TYPE;
    if (info.count >= output_capacity) return AIRPLAY_BPLIST_OUTPUT_TOO_SMALL;
    for (size_t i = 0U; i < (size_t)info.count; ++i) {
        uint8_t value = doc->bytes[info.payload_offset + i];
        if (info.type == 0x06U) {
            if (doc->bytes[info.payload_offset + 2U * i] != 0U) {
                return AIRPLAY_BPLIST_WRONG_TYPE;
            }
            value = doc->bytes[info.payload_offset + 2U * i + 1U];
        }
        if (value > 0x7fU || value == 0U) return AIRPLAY_BPLIST_WRONG_TYPE;
        output[i] = (char)value;
    }
    output[info.count] = '\0';
    *output_len = (size_t)info.count;
    return AIRPLAY_BPLIST_OK;
}

airplay_bplist_result_t airplay_bplist_get_data(
    const airplay_bplist_doc_t *doc,
    airplay_bplist_object_t object,
    const uint8_t **bytes,
    size_t *length)
{
    if (bytes == NULL || length == NULL) return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    *bytes = NULL;
    *length = 0U;
    bplist_object_info_t info;
    airplay_bplist_result_t result = resolve_object(doc, object, &info);
    if (result != AIRPLAY_BPLIST_OK) return result;
    if (info.type != 0x04U) return AIRPLAY_BPLIST_WRONG_TYPE;
    *bytes = doc->bytes + info.payload_offset;
    *length = info.payload_bytes;
    return AIRPLAY_BPLIST_OK;
}

typedef enum {
    BPLIST_WRITE_STRING,
    BPLIST_WRITE_INTEGER,
    BPLIST_WRITE_ARRAY,
    BPLIST_WRITE_DICTIONARY,
} bplist_write_kind_t;

typedef struct {
    bplist_write_kind_t kind;
    const char *text;
    uint64_t number;
    uint8_t references[12];
    uint8_t reference_count;
} bplist_write_object_t;

static uint8_t integer_width(uint64_t value)
{
    if (value <= UINT8_MAX) return 1U;
    if (value <= UINT16_MAX) return 2U;
    if (value <= UINT32_MAX) return 4U;
    return 8U;
}

static bool write_be(uint8_t *output, size_t capacity, size_t *offset,
                     uint64_t value, uint8_t width)
{
    if (output == NULL || offset == NULL || !supported_width(width) ||
        *offset > capacity || width > capacity - *offset) return false;
    for (uint8_t i = 0U; i < width; ++i) {
        uint8_t shift = (uint8_t)((width - i - 1U) * 8U);
        output[*offset + i] = (uint8_t)(value >> shift);
    }
    *offset += width;
    return true;
}

static bool emit_write_object(const bplist_write_object_t *object,
                              uint8_t reference_width,
                              uint8_t *output,
                              size_t capacity,
                              size_t *offset)
{
    if (object == NULL || output == NULL || offset == NULL) return false;
    switch (object->kind) {
    case BPLIST_WRITE_STRING: {
        size_t length = strlen(object->text);
        if (*offset >= capacity) return false;
        if (length < 15U) {
            output[(*offset)++] = (uint8_t)(0x50U | (uint8_t)length);
        } else {
            /* Binary plist uses a following integer object when the count nibble is 0xf. */
            output[(*offset)++] = 0x5fU;
            uint8_t width = integer_width((uint64_t)length);
            uint8_t power = width == 1U ? 0U : width == 2U ? 1U : width == 4U ? 2U : 3U;
            if (*offset >= capacity) return false;
            output[(*offset)++] = (uint8_t)(0x10U | power);
            if (!write_be(output, capacity, offset, (uint64_t)length, width)) return false;
        }
        if (length > capacity - *offset) return false;
        memcpy(output + *offset, object->text, length);
        *offset += length;
        return true;
    }
    case BPLIST_WRITE_INTEGER: {
        uint8_t width = integer_width(object->number);
        uint8_t power = width == 1U ? 0U : width == 2U ? 1U : width == 4U ? 2U : 3U;
        if (*offset >= capacity) return false;
        output[(*offset)++] = (uint8_t)(0x10U | power);
        return write_be(output, capacity, offset, object->number, width);
    }
    case BPLIST_WRITE_ARRAY:
    case BPLIST_WRITE_DICTIONARY: {
        uint8_t nibble = object->kind == BPLIST_WRITE_DICTIONARY
                             ? (uint8_t)(object->reference_count / 2U)
                             : object->reference_count;
        if (nibble >= 15U || *offset >= capacity) return false;
        output[(*offset)++] = (uint8_t)(
            (object->kind == BPLIST_WRITE_DICTIONARY ? 0xd0U : 0xa0U) | nibble);
        for (uint8_t i = 0U; i < object->reference_count; ++i) {
            if (!write_be(output, capacity, offset, object->references[i],
                          reference_width)) return false;
        }
        return true;
    }
    }
    return false;
}

static airplay_bplist_result_t build_setup_response(
    bplist_write_object_t *objects,
    uint8_t object_count,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len)
{
    static const uint8_t magic[BPLIST_HEADER_BYTES] = {
        'b', 'p', 'l', 'i', 's', 't', '0', '0',
    };
    if (output_len != NULL) *output_len = 0U;
    if (objects == NULL || object_count == 0U || output == NULL ||
        output_len == NULL) return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    if (output_capacity < BPLIST_HEADER_BYTES + BPLIST_TRAILER_BYTES) {
        return AIRPLAY_BPLIST_OUTPUT_TOO_SMALL;
    }
    size_t offset = 0U;
    memcpy(output, magic, sizeof(magic));
    offset += sizeof(magic);
    if (object_count > BPLIST_WRITER_MAX_OBJECTS) return AIRPLAY_BPLIST_TOO_LARGE;
    uint8_t object_offsets[BPLIST_WRITER_MAX_OBJECTS];
    for (uint8_t i = 0U; i < object_count; ++i) {
        if (offset > UINT8_MAX) return AIRPLAY_BPLIST_TOO_LARGE;
        object_offsets[i] = (uint8_t)offset;
        if (!emit_write_object(&objects[i], 1U, output, output_capacity, &offset)) {
            return AIRPLAY_BPLIST_OUTPUT_TOO_SMALL;
        }
    }
    if (offset > UINT8_MAX) return AIRPLAY_BPLIST_TOO_LARGE;
    size_t table_offset = offset;
    for (uint8_t i = 0U; i < object_count; ++i) {
        if (!write_be(output, output_capacity, &offset, object_offsets[i], 1U)) {
            return AIRPLAY_BPLIST_OUTPUT_TOO_SMALL;
        }
    }
    if (output_capacity - offset < BPLIST_TRAILER_BYTES) {
        return AIRPLAY_BPLIST_OUTPUT_TOO_SMALL;
    }
    memset(output + offset, 0, BPLIST_TRAILER_BYTES);
    output[offset + 6U] = 1U; /* offset byte width */
    output[offset + 7U] = 1U; /* object reference byte width */
    size_t trailer_offset = offset + 8U;
    offset += BPLIST_TRAILER_BYTES;
    if (!write_be(output, output_capacity, &trailer_offset, object_count, 8U) ||
        !write_be(output, output_capacity, &trailer_offset, 0U, 8U) ||
        !write_be(output, output_capacity, &trailer_offset, table_offset, 8U)) {
        return AIRPLAY_BPLIST_OUTPUT_TOO_SMALL;
    }
    *output_len = offset;
    return AIRPLAY_BPLIST_OK;
}

airplay_bplist_result_t airplay_bplist_build_initial_setup_response(
    uint16_t timing_port,
    uint16_t event_port,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len)
{
    if (timing_port == 0U || event_port == 0U) {
        return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    }
    bplist_write_object_t objects[9] = {
        {.kind = BPLIST_WRITE_DICTIONARY,
         /* Binary-plist dictionaries store all key references before values. */
         .references = {1U, 3U, 5U, 2U, 4U, 6U},
         .reference_count = 6U},
        {.kind = BPLIST_WRITE_STRING, .text = "timingPort"},
        {.kind = BPLIST_WRITE_INTEGER, .number = timing_port},
        {.kind = BPLIST_WRITE_STRING, .text = "eventPort"},
        {.kind = BPLIST_WRITE_INTEGER, .number = event_port},
        {.kind = BPLIST_WRITE_STRING, .text = "enabledFeatures"},
        {.kind = BPLIST_WRITE_ARRAY, .references = {7U, 8U},
         .reference_count = 2U},
        {.kind = BPLIST_WRITE_STRING, .text = "iAPChannel"},
        {.kind = BPLIST_WRITE_STRING, .text = "viewAreas"},
    };
    return build_setup_response(objects, 9U, output, output_capacity, output_len);
}

airplay_bplist_result_t airplay_bplist_build_screen_setup_response(
    uint64_t stream_type,
    uint16_t data_port,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len)
{
    if (stream_type > UINT32_MAX || data_port == 0U) {
        return AIRPLAY_BPLIST_INVALID_ARGUMENT;
    }
    bplist_write_object_t objects[8] = {
        {.kind = BPLIST_WRITE_DICTIONARY, .references = {1U, 2U},
         .reference_count = 2U},
        {.kind = BPLIST_WRITE_STRING, .text = "streams"},
        {.kind = BPLIST_WRITE_ARRAY, .references = {3U}, .reference_count = 1U},
        {.kind = BPLIST_WRITE_DICTIONARY,
         .references = {4U, 5U, 6U, 7U}, .reference_count = 4U},
        {.kind = BPLIST_WRITE_STRING, .text = "type"},
        {.kind = BPLIST_WRITE_STRING, .text = "dataPort"},
        {.kind = BPLIST_WRITE_INTEGER, .number = stream_type},
        {.kind = BPLIST_WRITE_INTEGER, .number = data_port},
    };
    return build_setup_response(objects, 8U, output, output_capacity, output_len);
}

airplay_bplist_result_t airplay_bplist_build_empty_stream_setup_response(
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len)
{
    bplist_write_object_t objects[3] = {
        {.kind = BPLIST_WRITE_DICTIONARY, .references = {1U, 2U},
         .reference_count = 2U},
        {.kind = BPLIST_WRITE_STRING, .text = "streams"},
        {.kind = BPLIST_WRITE_ARRAY, .reference_count = 0U},
    };
    return build_setup_response(objects, 3U, output, output_capacity, output_len);
}
