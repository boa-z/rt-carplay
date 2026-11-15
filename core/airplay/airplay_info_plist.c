/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal binary property-list writer for the AirPlay /info response.
 * Optional Vehicle catalogue follows LIVI by Lasse Heitgres; see NOTICE.
 *
 * Objects are emitted depth-first: children first, then their parent container.
 * Object IDs are assigned in emission order and the offset table maps IDs back
 * to the recorded positions, which is valid bplist even though IDs are not in
 * document order (the format only requires the trailer to name the top object).
 */
#include "airplay_info_plist.h"
#include "../audio/include/audio_codec.h"

#include <string.h>
#include <stdlib.h>
#include "airplay_bplist.h"
#include "rtcp/opack.h"
#include "rtcp/rtcp.h"

#include "airplay_hid_descriptor.h"
#include "rtcp/log.h"

#define TAG "info_plist"

/* The audio capability arrays push the document past 255 objects, so object
 * references are two bytes wide.  384 leaves headroom above the current ~290
 * objects (145 for the display/input document plus ~145 audio entries). */
#define IP_MAX_OBJECTS 384U
#define IP_REF_BYTES 2U /* IP_MAX_OBJECTS < 65536 */

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
    uint32_t *offsets;
    size_t max_objects;
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
    if (!w->ok || w->next_id >= w->max_objects) {
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

#define IP_MAX_DICT_ENTRIES 20U

static uint32_t ip_emit_dict(ip_writer_t *w, const uint32_t *keys,
                             const uint32_t *values, size_t count)
{
    uint32_t refs[2U * IP_MAX_DICT_ENTRIES];
    if (count > IP_MAX_DICT_ENTRIES) {
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
    uint32_t key_ids[IP_MAX_DICT_ENTRIES];
    if (count > IP_MAX_DICT_ENTRIES) {
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

/* Ported from the GPL-3.0 playport /info audio declarations
 * (AirPlayInfoPlist.kt): the same latency table keeps the CarPlay sender from
 * refusing the session for a missing stream description. */
static uint32_t emit_audio_latency(ip_writer_t *w, int64_t type,
                                   const char *audio_type)
{
    if (audio_type == NULL) {
        static const char *const keys[] = {
            "type", "inputLatencyMicros", "outputLatencyMicros",
        };
        uint32_t values[3];
        values[0] = ip_emit_int(w, type);
        values[1] = ip_emit_int(w, 0);
        values[2] = ip_emit_int(w, 0);
        return ip_emit_literal_dict(w, keys, values, 3U);
    }
    static const char *const keys[] = {
        "type", "audioType", "inputLatencyMicros", "outputLatencyMicros",
    };
    uint32_t values[4];
    values[0] = ip_emit_int(w, type);
    values[1] = ip_emit_string(w, audio_type);
    values[2] = ip_emit_int(w, 0);
    values[3] = ip_emit_int(w, 0);
    return ip_emit_literal_dict(w, keys, values, 4U);
}

static uint32_t emit_audio_latencies(ip_writer_t *w)
{
    uint32_t refs[9];
    refs[0] = emit_audio_latency(w, 100, NULL);
    refs[1] = emit_audio_latency(w, 100, "default");
    refs[2] = emit_audio_latency(w, 100, "media");
    refs[3] = emit_audio_latency(w, 100, "telephony");
    refs[4] = emit_audio_latency(w, 100, "speechRecognition");
    refs[5] = emit_audio_latency(w, 100, "alert");
    refs[6] = emit_audio_latency(w, 101, NULL);
    refs[7] = emit_audio_latency(w, 101, "default");
    /* Match the type/role of the media format, as in catplay's receiver. */
    refs[8] = emit_audio_latency(w, 102, "media");
    return ip_emit_array(w, refs, 9U);
}

static uint32_t emit_audio_format(ip_writer_t *w, int64_t type,
                                  const char *audio_type, uint64_t formats, uint64_t input_formats)
{
    static const char *const keys[] = {
        "type", "audioType", "audioOutputFormats", "audioInputFormats",
    };
    uint32_t values[4];
    values[0] = ip_emit_int(w, type);
    values[1] = ip_emit_string(w, audio_type);
    values[2] = ip_emit_int(w, (int64_t)formats);
    if (input_formats) values[3] = ip_emit_int(w, (int64_t)input_formats);
    return ip_emit_literal_dict(w, keys, values, input_formats ? 4U : 3U);
}

static uint32_t emit_audio_formats(ip_writer_t *w,
                                   const airplay_info_plist_config_t *config)
{
    /* Advertise only compiled decoders; microphone formats additionally
     * require the platform capture capability. 102/media is raw AAC-LC. */
    const bool is48 = config->audio_sample_rate == 48000U;
    const bool both = config->audio_sample_rate == 0U;
    const uint64_t pcm = 0x3fcULL | (both ? 0xcc00ULL : is48 ? 0xc000ULL : 0xc00ULL);
    const uint64_t pcm_mono = 0x154ULL | (both ? 0x4400ULL : is48 ? 0x4000ULL : 0x400ULL);

    uint64_t compiled = rtcp_audio_output_formats();
    uint64_t opus = config->audio_compressed ? compiled & 0x70000000ULL : 0;
    uint64_t aac = config->audio_compressed ? compiled & (both ? 0xc00000ULL : is48 ? 0x800000ULL : 0x400000ULL) : 0;
    uint64_t input = config->audio_input ? pcm_mono | opus : 0;
    uint32_t refs[9];
    refs[0] = emit_audio_format(w, 100, "compatibility", pcm, input);
    refs[1] = emit_audio_format(w, 101, "compatibility", pcm, 0);
    refs[2] = emit_audio_format(w, 100, "default", pcm | opus, input);
    refs[3] = emit_audio_format(w, 100, "alert", pcm | opus, 0);
    refs[4] = emit_audio_format(w, 100, "media", pcm, 0);
    refs[5] = emit_audio_format(w, 100, "telephony", pcm_mono | opus, input);
    refs[6] = emit_audio_format(w, 100, "speechRecognition", pcm_mono | opus, input);
    refs[7] = emit_audio_format(w, 101, "default", pcm | opus, 0);
    size_t count = 8;
    if (aac) refs[count++] = emit_audio_format(w, 102, "media", aac, 0);
    return ip_emit_array(w, refs, count);
}

static uint32_t emit_safe_area(ip_writer_t *w, const rtcp_display_rect_t *r,
                               bool draw_outside)
{
    static const char *const keys[] = {
        "widthPixels", "heightPixels", "originXPixels", "originYPixels",
        "drawUIOutsideSafeArea",
    };
    uint32_t values[5];
    values[0] = ip_emit_int(w, r->width);
    values[1] = ip_emit_int(w, r->height);
    values[2] = ip_emit_int(w, r->x);
    values[3] = ip_emit_int(w, r->y);
    values[4] = ip_emit_bool(w, draw_outside);
    return ip_emit_literal_dict(w, keys, values, 5U);
}

static uint32_t emit_view_area(ip_writer_t *w, const airplay_info_plist_config_t *config)
{
    static const char *const keys[] = {
        "widthPixels", "heightPixels", "originXPixels", "originYPixels",
        "safeArea",
    };
    const rtcp_display_profile_t *p = config->main_display_profile;
    rtcp_display_rect_t view = {0, 0, config->width_pixels, config->height_pixels};
    if (p != NULL && p->view_area.width != 0U) view = p->view_area;
    rtcp_display_rect_t safe = view;
    if (p != NULL && p->safe_area.width != 0U) safe = p->safe_area;
    uint32_t values[5];
    values[0] = ip_emit_int(w, view.width);
    values[1] = ip_emit_int(w, view.height);
    values[2] = ip_emit_int(w, view.x);
    values[3] = ip_emit_int(w, view.y);
    values[4] = emit_safe_area(w, &safe, p == NULL || p->draw_outside_safe_area);
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

    const rtcp_display_profile_t *p = config->main_display_profile;
    uint32_t view_area = emit_view_area(w, config);
    uint32_t view_areas = ip_emit_array(w, &view_area, 1U);

    uint32_t values[15];
    values[0] = ip_emit_string(w, p != NULL && p->uuid != NULL ? p->uuid : IP_DISPLAY_UUID);
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
    values[9] = ip_emit_int(w, p != NULL && p->ui_night);
    values[10] = ip_emit_int(w, p != NULL ? p->appearance_setting : 0);
    values[11] = ip_emit_int(w, p != NULL && p->map_night);
    values[12] = ip_emit_int(w, p != NULL ? p->appearance_setting : 0);
    values[13] = view_areas;
    values[14] = ip_emit_int(w, 0);
    return ip_emit_literal_dict(w, keys, values, 15U);
}

static uint32_t emit_hid_device(ip_writer_t *w, const airplay_info_plist_config_t *config,
                                const uint8_t *descriptor,
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
    const rtcp_display_profile_t *p = config->main_display_profile;
    values[5] = ip_emit_string(w, p != NULL && p->uuid != NULL ? p->uuid : IP_DISPLAY_UUID);
    values[6] = ip_emit_data(w, descriptor, descriptor_len);
    return ip_emit_literal_dict(w, keys, values, 7U);
}

/* Only generated catalogue nodes enter here. Large arenas live on the heap,
 * keeping the Classic RTSP owner's stack and byte-for-byte output unchanged. */
#define IP_VEHICLE_OPACK_BYTES 16384U
typedef struct {
    uint8_t opack[IP_VEHICLE_OPACK_BYTES];
    rtcp_opack_node_t nodes[RTCP_OPACK_MAX_NODES];
    uint32_t ids[RTCP_OPACK_MAX_NODES];
    uint32_t offsets[IP_MAX_OBJECTS + RTCP_OPACK_MAX_NODES + 1U];
} ip_vehicle_workspace_t;

static bool vehicle_nodes(rtcp_vehicle_t *vehicle, ip_vehicle_workspace_t *v, size_t *count)
{
    size_t length = 0;
    return rtcp_vehicle_info(vehicle,v->opack,sizeof(v->opack),&length) == RTCP_OK &&
        rtcp_opack_decode(v->opack,length,v->nodes,RTCP_OPACK_MAX_NODES,count) == RTCP_OPACK_OK;
}

static uint32_t emit_vehicle(ip_writer_t *w, const rtcp_opack_node_t *nodes, size_t count,
    uint32_t *ids)
{
    uint32_t refs[2U * IP_MAX_DICT_ENTRIES];
    for (size_t i=count; i-- > 0 && w->ok;) {
        const rtcp_opack_node_t *n=&nodes[i];
        switch (n->type) {
        case RTCP_OPACK_BOOL: ids[i]=ip_emit_bool(w,n->value.boolean); break;
        case RTCP_OPACK_INT: ids[i]=ip_emit_int(w,n->value.integer); break;
        case RTCP_OPACK_REAL: {
            uint64_t bits; memcpy(&bits,&n->value.real,sizeof(bits));
            ids[i]=ip_object_start(w); ip_u8(w,IP_MARKER_REAL); ip_be(w,bits,8); break;
        }
        case RTCP_OPACK_STRING: {
            const uint8_t *begin=n->value.blob.bytes, *end=begin+n->value.blob.length;
            bool ascii=true; size_t units=0;
            for (const uint8_t *p=begin;p<end;) {
                uint32_t cp=ip_utf8_decode(&p,end); if (cp > 127U) ascii=false;
                units+=cp > 0xffffU ? 2U : 1U;
            }
            if (ascii) ids[i]=ip_emit_ascii(w,(const char *)begin,n->value.blob.length);
            else {
                ids[i]=ip_object_start(w);
                if (units < IP_LONG) ip_u8(w,(uint8_t)(IP_MARKER_UTF16|units));
                else { ip_u8(w,IP_MARKER_UTF16|IP_LONG); ip_int_inline(w,(int64_t)units); }
                for (const uint8_t *p=begin;p<end;) {
                    uint32_t cp=ip_utf8_decode(&p,end);
                    if (cp <= 0xffffU) ip_be(w,cp,2);
                    else { cp-=0x10000U; ip_be(w,0xd800U|(cp>>10),2); ip_be(w,0xdc00U|(cp&0x3ffU),2); }
                }
            }
            break;
        }
        case RTCP_OPACK_DATA: ids[i]=ip_emit_data(w,n->value.blob.bytes,n->value.blob.length); break;
        case RTCP_OPACK_ARRAY: case RTCP_OPACK_DICT: {
            bool dict=n->type == RTCP_OPACK_DICT;
            if (n->count > IP_MAX_DICT_ENTRIES) { w->ok=false; break; }
            size_t child=i+1;
            for (unsigned j=0;j<n->count;++j) {
                refs[j]=ids[child]; child=nodes[child].end;
                if (dict) { refs[n->count+j]=ids[child]; child=nodes[child].end; }
            }
            ids[i]=ip_emit_container(w,dict ? IP_MARKER_DICT : IP_MARKER_ARRAY,
                n->count,refs,n->count*(dict ? 2U : 1U)); break;
        }
        default: w->ok=false; break;
        }
    }
    return w->ok ? ids[0] : 0;
}

static uint32_t emit_root(ip_writer_t *w,
                          const airplay_info_plist_config_t *config,
                          const char *name,
                          const uint8_t *descriptor,
                          size_t descriptor_len,
                          uint32_t vehicle_info)
{
    static const char *const base_keys[] = {
        "sourceVersion", "features", "statusFlags", "model", "manufacturer",
        "deviceID", "bluetoothIDs", "name", "rightHandDrive",
        "keepAliveLowPower", "keepAliveSendStatsAsBody", "modes",
        "extendedFeatures", "displays", "hidDevices",
    };
    const char *keys[18];
    memcpy(keys, base_keys, sizeof(base_keys));

    uint32_t bluetooth;
    if (config->bluetooth_id != NULL && config->bluetooth_id[0] != '\0') {
        uint32_t id = ip_emit_string(w, config->bluetooth_id);
        bluetooth = ip_emit_array(w, &id, 1U);
    } else {
        bluetooth = ip_emit_array(w, NULL, 0U);
    }
    uint32_t display = emit_display(w, config);
    uint32_t displays = ip_emit_array(w, &display, 1U);
    uint32_t hid = emit_hid_device(w, config, descriptor, descriptor_len);
    uint32_t hids = ip_emit_array(w, &hid, 1U);
    uint32_t modes = emit_modes(w);
    uint32_t extended = emit_extended_features(w);

    uint32_t values[18];
    values[0] = ip_emit_string(w, config->source_version != NULL
                                      ? config->source_version
                                      : AIRPLAY_INFO_DEFAULT_SOURCE_VERSION);
    values[1] = ip_emit_int(w, (int64_t)airplay_info_plist_features(config));
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
    size_t entry_count = 15U;
    if (config->audio_output) {
        keys[15] = "audioLatencies";
        values[15] = emit_audio_latencies(w);
        keys[16] = "audioFormats";
        values[16] = emit_audio_formats(w, config);
        entry_count = 17U;
    }

    if (config->vehicle) {
        keys[entry_count]="vehicleStateProtocolInfo";
        values[entry_count++]=vehicle_info;
    }
    return ip_emit_literal_dict(w, keys, values, entry_count);
}

bool airplay_info_plist_build(const airplay_info_plist_config_t *config,
                              uint8_t *output,
                              size_t output_capacity,
                              size_t *output_len)
{
    if (output_len != NULL) *output_len = 0U;
    if (config == NULL || config->device_id == NULL ||
        config->device_id[0] == '\0' || config->width_pixels == 0U ||
        config->height_pixels == 0U || output == NULL || output_len == NULL ||
        !rtcp_display_profile_validate(config->main_display_profile,
                                       config->width_pixels, config->height_pixels)) {
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

    if (output_capacity < 8U) return false;
    ip_vehicle_workspace_t *vehicle = config->vehicle ? calloc(1,sizeof(*vehicle)) : NULL;
    if (config->vehicle && !vehicle) return false;
    uint32_t classic_offsets[IP_MAX_OBJECTS];
    ip_writer_t writer;
    memset(&writer, 0, sizeof(writer));
    writer.out = output;
    writer.capacity = config->vehicle && output_capacity > AIRPLAY_INFO_PLIST_MAX_BYTES
        ? AIRPLAY_INFO_PLIST_MAX_BYTES : output_capacity;
    writer.ok = true;
    writer.offsets = vehicle ? vehicle->offsets : classic_offsets;
    writer.max_objects = vehicle ? sizeof(vehicle->offsets)/sizeof(vehicle->offsets[0]) : IP_MAX_OBJECTS;

    ip_bytes(&writer, "bplist00", 8U);

    const char *name = config->name != NULL && config->name[0] != '\0'
                           ? config->name
                           : IP_DEFAULT_NAME;
    uint32_t vehicle_info=0; size_t node_count=0;
    if (vehicle) {
        if (!vehicle_nodes(config->vehicle,vehicle,&node_count)) { free(vehicle); return false; }
        vehicle_info=emit_vehicle(&writer,vehicle->nodes,node_count,vehicle->ids);
    }
    uint32_t root = emit_root(&writer, config, name, descriptor, descriptor_len,vehicle_info);
    if (!writer.ok) {
        RTCP_LOGE(TAG, "encode failed objects=%u used=%u",
                  (unsigned)writer.next_id, (unsigned)writer.used);
        free(vehicle); return false;
    }

    size_t table_offset = writer.used;
    for (uint32_t i = 0U; i < writer.next_id; ++i) {
        ip_be(&writer, writer.offsets[i], 4U);
    }
    if (!writer.ok) { free(vehicle); return false; }

    static const uint8_t zeros[6] = {0};
    ip_bytes(&writer, zeros, sizeof(zeros));
    ip_u8(&writer, 4U);                                     /* offset size */
    ip_u8(&writer, IP_REF_BYTES);                           /* ref size */
    ip_be(&writer, writer.next_id, 8U);                     /* num objects */
    ip_be(&writer, root, 8U);                               /* top object */
    ip_be(&writer, table_offset, 8U);                       /* offset table */
    if (!writer.ok) { free(vehicle); return false; }

    free(vehicle);
    *output_len = writer.used;
    RTCP_LOGD(TAG, "built /info plist bytes=%u objects=%u",
              (unsigned)writer.used, (unsigned)writer.next_id);
    return true;
}

uint64_t airplay_info_plist_features(const airplay_info_plist_config_t *config)
{
    if (config && config->features) return config->features;
    if (!config || !config->audio_output) return AIRPLAY_INFO_DEFAULT_FEATURES;
    uint64_t features = AIRPLAY_INFO_DEFAULT_FEATURES_AUDIO;
    if (config->audio_compressed && (rtcp_audio_output_formats() & UINT64_C(0xc00000)))
        features |= AIRPLAY_INFO_AAC_LC_FEATURE;
    return features;
}

static bool matches_node(const airplay_bplist_doc_t *doc, airplay_bplist_object_t object,
    const rtcp_opack_node_t *nodes, size_t at, unsigned depth)
{
    if (depth > RTCP_OPACK_MAX_DEPTH) return false;
    const rtcp_opack_node_t *n=&nodes[at];
    airplay_bplist_type_t type; uint64_t count;
    if (airplay_bplist_get_type(doc,object,&type,&count) != AIRPLAY_BPLIST_OK) return false;
    switch (n->type) {
    case RTCP_OPACK_BOOL: {
        bool value;
        return airplay_bplist_get_bool(doc,object,&value) == AIRPLAY_BPLIST_OK && value == n->value.boolean;
    }
    case RTCP_OPACK_INT: {
        uint64_t value;
        return airplay_bplist_get_uint(doc,object,&value) == AIRPLAY_BPLIST_OK && value == (uint64_t)n->value.integer;
    }
    case RTCP_OPACK_REAL: {
        double value;
        return type == AIRPLAY_BPLIST_REAL && airplay_bplist_get_number(doc,object,&value) == AIRPLAY_BPLIST_OK && value == n->value.real;
    }
    case RTCP_OPACK_STRING:
        return airplay_bplist_string_equals_utf8(doc,object,n->value.blob.bytes,n->value.blob.length);
    case RTCP_OPACK_DATA: {
        const uint8_t *bytes; size_t length;
        return airplay_bplist_get_data(doc,object,&bytes,&length) == AIRPLAY_BPLIST_OK &&
            length == n->value.blob.length && !memcmp(bytes,n->value.blob.bytes,length);
    }
    case RTCP_OPACK_ARRAY: case RTCP_OPACK_DICT: {
        bool dict=n->type == RTCP_OPACK_DICT;
        if (type != (dict ? AIRPLAY_BPLIST_DICTIONARY : AIRPLAY_BPLIST_ARRAY) || count != n->count) return false;
        size_t child=at+1;
        for (uint64_t i=0;i<count;++i) {
            airplay_bplist_object_t value;
            if (dict) {
                /* Generated catalogue keys are short ASCII except the plugin
                 * mapping name; use the UTF-8-aware lookup below for that. */
                const rtcp_opack_node_t *key=&nodes[child];
                if (key->type != RTCP_OPACK_STRING ||
                    airplay_bplist_dict_get_utf8_unique(doc,object,key->value.blob.bytes,
                        key->value.blob.length,&value) != AIRPLAY_BPLIST_OK) return false;
                child=key->end;
            } else if (airplay_bplist_array_get(doc,object,i,&value) != AIRPLAY_BPLIST_OK) return false;
            if (!matches_node(doc,value,nodes,child,depth+1)) return false;
            child=nodes[child].end;
        }
        return true;
    }
    default: return false;
    }
}

bool airplay_info_plist_matches_vehicle(const uint8_t *bytes, size_t length, rtcp_vehicle_t *model)
{
    if (!bytes || !model || length > AIRPLAY_INFO_PLIST_MAX_BYTES) return false;
    airplay_bplist_doc_t doc; airplay_bplist_object_t root,info;
    if (airplay_bplist_open(bytes,length,&doc) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_root(&doc,&root) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_dict_get_unique(&doc,root,"vehicleStateProtocolInfo",&info) != AIRPLAY_BPLIST_OK) return false;
    ip_vehicle_workspace_t *workspace=calloc(1,sizeof(*workspace));
    if (!workspace) return false;
    size_t count=0;
    bool matches=vehicle_nodes(model,workspace,&count) && matches_node(&doc,info,workspace->nodes,0,0);
    free(workspace);
    return matches;
}
