/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal binary property-list writer for the AirPlay /info response.
 *
 * Objects are emitted depth-first: children first, then their parent container.
 * Object IDs are assigned in emission order and the offset table maps IDs back
 * to the recorded positions, which is valid bplist even though IDs are not in
 * document order (the format only requires the trailer to name the top object).
 */
#include "airplay_info_plist.h"

#include <string.h>

#include "airplay_hid_descriptor.h"
#include "rtcp/log.h"

#define TAG "info_plist"

#define IP_MAX_OBJECTS 192U
#define IP_REF_BYTES 1U /* IP_MAX_OBJECTS < 256 */

#define IP_MARKER_INT 0x10U
#define IP_MARKER_REAL 0x23U
#define IP_MARKER_DATA 0x40U
#define IP_MARKER_ASCII 0x50U
#define IP_MARKER_UTF16 0x60U
#define IP_MARKER_ARRAY 0xA0U
#define IP_MARKER_DICT 0xD0U
#define IP_LONG 0x0FU

#define IP_DISPLAY_UUID "b7e6c5a0-1111-4000-8000-000000000001"
#define IP_HID_UUID "2a2a2a2a"
#define IP_HID_NAME "rt-carplay Touchscreen"
#define IP_DEFAULT_NAME "rt-carplay"
#define IP_DEFAULT_WIDTH_PHYSICAL 200U
#define IP_DEFAULT_HEIGHT_PHYSICAL 120U
#define IP_DEFAULT_MAX_FPS 10U
#define IP_DISPLAY_TYPE 110U
#define IP_DISPLAY_FEATURES 0x08U

typedef struct {
    uint8_t *out;
    size_t capacity;
    size_t used;
    uint32_t offsets[IP_MAX_OBJECTS];
    uint32_t next_id;
    bool ok;
} ip_writer_t;

static void ip_reserve(ip_writer_t *w, size_t bytes)
{
    if (!w->ok) return;
    if (bytes > w->capacity - w->used) {
        w->ok = false;
        return;
    }
}

static void ip_u8(ip_writer_t *w, uint8_t value)
{
    ip_reserve(w, 1U);
    if (!w->ok) return;
    w->out[w->used++] = value;
}

static void ip_be(ip_writer_t *w, uint64_t value, size_t bytes)
{
    ip_reserve(w, bytes);
    if (!w->ok) return;
    for (size_t i = 0U; i < bytes; ++i) {
        w->out[w->used + bytes - 1U - i] = (uint8_t)(value >> (8U * i));
    }
    w->used += bytes;
}

static void ip_bytes(ip_writer_t *w, const void *data, size_t len)
{
    ip_reserve(w, len);
    if (!w->ok) return;
    memcpy(w->out + w->used, data, len);
    w->used += len;
}

static uint32_t ip_object_start(ip_writer_t *w)
{
    if (!w->ok || w->next_id >= IP_MAX_OBJECTS) {
        w->ok = false;
        return 0U;
    }
    uint32_t id = w->next_id++;
    w->offsets[id] = (uint32_t)w->used;
    return id;
}

/* Inline integer used for container counts; not an addressable object. */
static void ip_int_inline(ip_writer_t *w, int64_t value)
{
    /* Positive values use the shortest unsigned width.  Negative values are
     * always 8-byte two's complement: readers treat sub-8-byte integers as
     * unsigned (Python's plistlib does, and so does Apple's parser). */
    if (value < 0) {
        ip_u8(w, IP_MARKER_INT + 3U);
        ip_be(w, (uint64_t)value, 8U);
    } else if (value <= 0xFF) {
        ip_u8(w, IP_MARKER_INT + 0U);
        ip_u8(w, (uint8_t)value);
    } else if (value <= 0xFFFF) {
        ip_u8(w, IP_MARKER_INT + 1U);
        ip_be(w, (uint64_t)value, 2U);
    } else if (value <= 0xFFFFFFFFLL) {
        ip_u8(w, IP_MARKER_INT + 2U);
        ip_be(w, (uint64_t)value, 4U);
    } else {
        ip_u8(w, IP_MARKER_INT + 3U);
        ip_be(w, (uint64_t)value, 8U);
    }
}

static uint32_t ip_emit_int(ip_writer_t *w, int64_t value)
{
    uint32_t id = ip_object_start(w);
    ip_int_inline(w, value);
    return id;
}

static uint32_t ip_emit_bool(ip_writer_t *w, bool value)
{
    uint32_t id = ip_object_start(w);
    ip_u8(w, value ? 0x09U : 0x08U);
    return id;
}

static uint32_t ip_emit_data(ip_writer_t *w, const uint8_t *data, size_t len)
{
    uint32_t id = ip_object_start(w);
    if (len < IP_LONG) {
        ip_u8(w, (uint8_t)(IP_MARKER_DATA | len));
    } else {
        ip_u8(w, IP_MARKER_DATA | IP_LONG);
        ip_int_inline(w, (int64_t)len);
    }
    ip_bytes(w, data, len);
    return id;
}

static uint32_t ip_emit_ascii(ip_writer_t *w, const char *text, size_t len)
{
    uint32_t id = ip_object_start(w);
    if (len < IP_LONG) {
        ip_u8(w, (uint8_t)(IP_MARKER_ASCII | len));
    } else {
        ip_u8(w, IP_MARKER_ASCII | IP_LONG);
        ip_int_inline(w, (int64_t)len);
    }
    ip_bytes(w, text, len);
    return id;
}

static bool ip_utf8_is_ascii(const char *text)
{
    for (const uint8_t *p = (const uint8_t *)text; *p != 0U; ++p) {
        if (*p >= 0x80U) return false;
    }
    return true;
}

static uint32_t ip_utf8_decode(const uint8_t **cursor, const uint8_t *end)
{
    const uint8_t *p = *cursor;
    uint8_t first = *p++;

    if (first < 0x80U) {
        *cursor = p;
        return first;
    }

    size_t extra;
    uint32_t value;
    uint32_t minimum;
    if ((first & 0xE0U) == 0xC0U) {
        extra = 1U;
        value = first & 0x1FU;
        minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
        extra = 2U;
        value = first & 0x0FU;
        minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
        extra = 3U;
        value = first & 0x07U;
        minimum = 0x10000U;
    } else {
        *cursor = p;
        return 0xFFFDU;
    }

    if ((size_t)(end - p) < extra) {
        *cursor = p;
        return 0xFFFDU;
    }
    for (size_t i = 0U; i < extra; ++i) {
        if ((p[i] & 0xC0U) != 0x80U) {
            *cursor = p;
            return 0xFFFDU;
        }
        value = (value << 6U) | (p[i] & 0x3FU);
    }
    *cursor = p + extra;

    if (value < minimum || value > 0x10FFFFU ||
        (value >= 0xD800U && value <= 0xDFFFU)) {
        return 0xFFFDU;
    }
    return value;
}

static size_t ip_utf16_units(const char *text)
{
    const uint8_t *cursor = (const uint8_t *)text;
    const uint8_t *end = cursor + strlen(text);
    size_t units = 0U;
    while (cursor < end) {
        uint32_t code = ip_utf8_decode(&cursor, end);
        units += code > 0xFFFFU ? 2U : 1U;
    }
    return units;
}

static uint32_t ip_emit_utf16(ip_writer_t *w, const char *text, size_t units)
{
    uint32_t id = ip_object_start(w);
    if (units < IP_LONG) {
        ip_u8(w, (uint8_t)(IP_MARKER_UTF16 | units));
    } else {
        ip_u8(w, IP_MARKER_UTF16 | IP_LONG);
        ip_int_inline(w, (int64_t)units);
    }

    const uint8_t *cursor = (const uint8_t *)text;
    const uint8_t *end = cursor + strlen(text);
    while (cursor < end) {
        uint32_t code = ip_utf8_decode(&cursor, end);
        if (code > 0xFFFFU) {
            uint32_t adjusted = code - 0x10000U;
            ip_be(w, 0xD800U | (adjusted >> 10U), 2U);
            ip_be(w, 0xDC00U | (adjusted & 0x3FFU), 2U);
        } else {
            ip_be(w, code, 2U);
        }
    }
    return id;
}

static uint32_t ip_emit_string(ip_writer_t *w, const char *text)
{
    if (text == NULL) text = "";
    if (ip_utf8_is_ascii(text)) {
        return ip_emit_ascii(w, text, strlen(text));
    }
    return ip_emit_utf16(w, text, ip_utf16_units(text));
}

/* entry_count is the value stored in the marker; dicts carry two refs per
 * entry, so ref_count differs from entry_count there. */
static uint32_t ip_emit_container(ip_writer_t *w, uint8_t marker,
                                  size_t entry_count,
                                  const uint32_t *refs, size_t ref_count)
{
    uint32_t id = ip_object_start(w);
    if (entry_count < IP_LONG) {
        ip_u8(w, (uint8_t)(marker | entry_count));
    } else {
        ip_u8(w, marker | IP_LONG);
        ip_int_inline(w, (int64_t)entry_count);
    }
    for (size_t i = 0U; i < ref_count; ++i) {
        if (refs[i] >= (1U << (8U * IP_REF_BYTES))) {
            w->ok = false;
            return id;
        }
        ip_be(w, refs[i], IP_REF_BYTES);
    }
    return id;
}

static uint32_t ip_emit_array(ip_writer_t *w, const uint32_t *refs, size_t count)
{
    return ip_emit_container(w, IP_MARKER_ARRAY, count, refs, count);
}

static uint32_t ip_emit_dict(ip_writer_t *w, const uint32_t *keys,
                             const uint32_t *values, size_t count)
{
    uint32_t refs[2U * 16U];
    if (count > 16U) {
        w->ok = false;
        return 0U;
    }
    for (size_t i = 0U; i < count; ++i) {
        refs[i] = keys[i];
        refs[count + i] = values[i];
    }
    return ip_emit_container(w, IP_MARKER_DICT, count, refs, count * 2U);
}

/* Emits a dict whose keys are fixed literals plus caller-supplied values. */
static uint32_t ip_emit_literal_dict(ip_writer_t *w, const char *const *keys,
                                     const uint32_t *values, size_t count)
{
    uint32_t key_ids[16];
    if (count > 16U) {
        w->ok = false;
        return 0U;
    }
    for (size_t i = 0U; i < count; ++i) {
        key_ids[i] = ip_emit_string(w, keys[i]);
    }
    return ip_emit_dict(w, key_ids, values, count);
}

static uint32_t emit_resource(ip_writer_t *w, uint64_t resource_id)
{
    static const char *const keys[] = {
        "resourceID", "transferType", "transferPriority",
        "takeConstraint", "borrowConstraint", "unborrowConstraint",
    };
    uint32_t values[6];
    values[0] = ip_emit_int(w, (int64_t)resource_id);
    values[1] = ip_emit_int(w, 1);
    values[2] = ip_emit_int(w, 100);
    values[3] = ip_emit_int(w, 100);
    values[4] = ip_emit_int(w, 100);
    values[5] = ip_emit_int(w, 100);
    return ip_emit_literal_dict(w, keys, values, 6U);
}

static uint32_t emit_app_state_bool(ip_writer_t *w, uint64_t app_state_id,
                                    bool state)
{
    static const char *const keys[] = {"appStateID", "state"};
    uint32_t values[2];
    values[0] = ip_emit_int(w, (int64_t)app_state_id);
    values[1] = ip_emit_bool(w, state);
    return ip_emit_literal_dict(w, keys, values, 2U);
}

static uint32_t emit_app_state_speech(ip_writer_t *w, uint64_t app_state_id,
                                      int64_t speech_mode)
{
    static const char *const keys[] = {"appStateID", "speechMode"};
    uint32_t values[2];
    values[0] = ip_emit_int(w, (int64_t)app_state_id);
    values[1] = ip_emit_int(w, speech_mode);
    return ip_emit_literal_dict(w, keys, values, 2U);
}

static uint32_t emit_modes(ip_writer_t *w)
{
    static const char *const keys[] = {"resources", "appStates"};

    uint32_t resources[2];
    resources[0] = emit_resource(w, 1U);
    resources[1] = emit_resource(w, 2U);
    uint32_t resource_array = ip_emit_array(w, resources, 2U);

    uint32_t states[3];
    states[0] = emit_app_state_bool(w, 2U, false);
    states[1] = emit_app_state_speech(w, 1U, -1);
    states[2] = emit_app_state_bool(w, 3U, false);
    uint32_t state_array = ip_emit_array(w, states, 3U);

    uint32_t values[2];
    values[0] = resource_array;
    values[1] = state_array;
    return ip_emit_literal_dict(w, keys, values, 2U);
}

static uint32_t emit_extended_features(ip_writer_t *w)
{
    uint32_t refs[2];
    refs[0] = ip_emit_string(w, "vocoderInfo");
    refs[1] = ip_emit_string(w, "enhancedRequestCarUI");
    return ip_emit_array(w, refs, 2U);
}

static uint32_t emit_safe_area(ip_writer_t *w, uint16_t width, uint16_t height)
{
    static const char *const keys[] = {
        "widthPixels", "heightPixels", "originXPixels", "originYPixels",
        "drawUIOutsideSafeArea",
    };
    uint32_t values[5];
    values[0] = ip_emit_int(w, width);
    values[1] = ip_emit_int(w, height);
    values[2] = ip_emit_int(w, 0);
    values[3] = ip_emit_int(w, 0);
    values[4] = ip_emit_bool(w, true);
    return ip_emit_literal_dict(w, keys, values, 5U);
}

static uint32_t emit_view_area(ip_writer_t *w, uint16_t width, uint16_t height)
{
    static const char *const keys[] = {
        "widthPixels", "heightPixels", "originXPixels", "originYPixels",
        "safeArea",
    };
    uint32_t values[5];
    values[0] = ip_emit_int(w, width);
    values[1] = ip_emit_int(w, height);
    values[2] = ip_emit_int(w, 0);
    values[3] = ip_emit_int(w, 0);
    values[4] = emit_safe_area(w, width, height);
    return ip_emit_literal_dict(w, keys, values, 5U);
}

static uint32_t emit_display(ip_writer_t *w,
                             const airplay_info_plist_config_t *config)
{
    static const char *const keys[] = {
        "uuid", "type", "maxFPS", "widthPixels", "heightPixels",
        "widthPhysical", "heightPhysical", "features", "primaryInputDevice",
        "uiAppearanceMode", "uiAppearanceSetting", "mapAppearanceMode",
        "mapAppearanceSetting", "viewAreas", "initialViewArea",
    };

    uint32_t view_area = emit_view_area(w, config->width_pixels,
                                        config->height_pixels);
    uint32_t view_areas = ip_emit_array(w, &view_area, 1U);

    uint32_t values[15];
    values[0] = ip_emit_string(w, IP_DISPLAY_UUID);
    values[1] = ip_emit_int(w, IP_DISPLAY_TYPE);
    values[2] = ip_emit_int(w, config->max_fps != 0U ? config->max_fps
                                                     : IP_DEFAULT_MAX_FPS);
    values[3] = ip_emit_int(w, config->width_pixels);
    values[4] = ip_emit_int(w, config->height_pixels);
    values[5] = ip_emit_int(w, config->width_physical_mm != 0U
                                   ? config->width_physical_mm
                                   : IP_DEFAULT_WIDTH_PHYSICAL);
    values[6] = ip_emit_int(w, config->height_physical_mm != 0U
                                   ? config->height_physical_mm
                                   : IP_DEFAULT_HEIGHT_PHYSICAL);
    values[7] = ip_emit_int(w, IP_DISPLAY_FEATURES);
    values[8] = ip_emit_int(w, 1);
    values[9] = ip_emit_int(w, 0);
    values[10] = ip_emit_int(w, 0);
    values[11] = ip_emit_int(w, 0);
    values[12] = ip_emit_int(w, 0);
    values[13] = view_areas;
    values[14] = ip_emit_int(w, 0);
    return ip_emit_literal_dict(w, keys, values, 15U);
}

static uint32_t emit_hid_device(ip_writer_t *w, const uint8_t *descriptor,
                                size_t descriptor_len)
{
    static const char *const keys[] = {
        "hidProductID", "hidVendorID", "hidCountryCode", "uuid", "name",
        "displayUUID", "hidDescriptor",
    };
    uint32_t values[7];
    values[0] = ip_emit_int(w, 1);
    values[1] = ip_emit_int(w, 2);
    values[2] = ip_emit_int(w, 0);
    values[3] = ip_emit_string(w, IP_HID_UUID);
    values[4] = ip_emit_string(w, IP_HID_NAME);
    values[5] = ip_emit_string(w, IP_DISPLAY_UUID);
    values[6] = ip_emit_data(w, descriptor, descriptor_len);
    return ip_emit_literal_dict(w, keys, values, 7U);
}

static uint32_t emit_root(ip_writer_t *w,
                          const airplay_info_plist_config_t *config,
                          const char *name,
                          const uint8_t *descriptor,
                          size_t descriptor_len)
{
    static const char *const keys[] = {
        "sourceVersion", "features", "statusFlags", "model", "manufacturer",
        "deviceID", "bluetoothIDs", "name", "rightHandDrive",
        "keepAliveLowPower", "keepAliveSendStatsAsBody", "modes",
        "extendedFeatures", "displays", "hidDevices",
    };

    uint32_t bluetooth;
    if (config->bluetooth_id != NULL && config->bluetooth_id[0] != '\0') {
        uint32_t id = ip_emit_string(w, config->bluetooth_id);
        bluetooth = ip_emit_array(w, &id, 1U);
    } else {
        bluetooth = ip_emit_array(w, NULL, 0U);
    }
    uint32_t display = emit_display(w, config);
    uint32_t displays = ip_emit_array(w, &display, 1U);
    uint32_t hid = emit_hid_device(w, descriptor, descriptor_len);
    uint32_t hids = ip_emit_array(w, &hid, 1U);
    uint32_t modes = emit_modes(w);
    uint32_t extended = emit_extended_features(w);

    uint32_t values[15];
    values[0] = ip_emit_string(w, config->source_version != NULL
                                      ? config->source_version
                                      : AIRPLAY_INFO_DEFAULT_SOURCE_VERSION);
    values[1] = ip_emit_int(w, (int64_t)(config->features != 0U
                                             ? config->features
                                             : AIRPLAY_INFO_DEFAULT_FEATURES));
    values[2] = ip_emit_int(w, config->status_flags != 0U
                                   ? config->status_flags
                                   : AIRPLAY_INFO_DEFAULT_STATUS_FLAGS);
    values[3] = ip_emit_string(w, config->model != NULL
                                      ? config->model
                                      : AIRPLAY_INFO_DEFAULT_MODEL);
    values[4] = ip_emit_string(w, config->manufacturer != NULL
                                      ? config->manufacturer
                                      : AIRPLAY_INFO_DEFAULT_MANUFACTURER);
    values[5] = ip_emit_string(w, config->device_id);
    values[6] = bluetooth;
    values[7] = ip_emit_string(w, name);
    values[8] = ip_emit_bool(w, config->right_hand_drive);
    values[9] = ip_emit_bool(w, false);
    values[10] = ip_emit_bool(w, false);
    values[11] = modes;
    values[12] = extended;
    values[13] = displays;
    values[14] = hids;

    return ip_emit_literal_dict(w, keys, values, 15U);
}

bool airplay_info_plist_build(const airplay_info_plist_config_t *config,
                              uint8_t *output,
                              size_t output_capacity,
                              size_t *output_len)
{
    if (output_len != NULL) *output_len = 0U;
    if (config == NULL || config->device_id == NULL ||
        config->device_id[0] == '\0' || config->width_pixels == 0U ||
        config->height_pixels == 0U || output == NULL || output_len == NULL) {
        return false;
    }

    uint8_t descriptor[AIRPLAY_HID_TOUCH_DESCRIPTOR_BYTES];
    size_t descriptor_len = 0U;
    if (!airplay_hid_touch_descriptor_build(config->width_pixels,
                                            config->height_pixels,
                                            descriptor, sizeof(descriptor),
                                            &descriptor_len)) {
        return false;
    }

    ip_writer_t writer;
    memset(&writer, 0, sizeof(writer));
    writer.out = output;
    writer.capacity = output_capacity;
    writer.ok = true;

    if (output_capacity < 8U) return false;
    ip_bytes(&writer, "bplist00", 8U);

    const char *name = config->name != NULL && config->name[0] != '\0'
                           ? config->name
                           : IP_DEFAULT_NAME;
    uint32_t root = emit_root(&writer, config, name, descriptor, descriptor_len);
    if (!writer.ok) {
        RTCP_LOGE(TAG, "encode failed objects=%u used=%u",
                  (unsigned)writer.next_id, (unsigned)writer.used);
        return false;
    }

    size_t table_offset = writer.used;
    for (uint32_t i = 0U; i < writer.next_id; ++i) {
        ip_be(&writer, writer.offsets[i], 4U);
    }
    if (!writer.ok) return false;

    static const uint8_t zeros[6] = {0};
    ip_bytes(&writer, zeros, sizeof(zeros));
    ip_u8(&writer, 4U);                                     /* offset size */
    ip_u8(&writer, IP_REF_BYTES);                           /* ref size */
    ip_be(&writer, writer.next_id, 8U);                     /* num objects */
    ip_be(&writer, root, 8U);                               /* top object */
    ip_be(&writer, table_offset, 8U);                       /* offset table */
    if (!writer.ok) return false;

    *output_len = writer.used;
    RTCP_LOGD(TAG, "built /info plist bytes=%u objects=%u",
              (unsigned)writer.used, (unsigned)writer.next_id);
    return true;
}
