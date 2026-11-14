/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtcp/plist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtcp/rtcp.h"

#define PLIST_HEADER                                                        \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                         \
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "              \
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"                \
    "<plist version=\"1.0\">\n"

#define PLIST_FOOTER "</plist>\n"

#define PLIST_KEY_MAX 64U

static bool append_bytes(rtcp_plist_writer_t *writer, const void *data,
                         size_t length)
{
    if (writer->failed || length > writer->capacity - writer->length) {
        writer->failed = true;
        return false;
    }
    memcpy(writer->buffer + writer->length, data, length);
    writer->length += length;
    return true;
}

static bool append_literal(rtcp_plist_writer_t *writer, const char *text)
{
    return append_bytes(writer, text, strlen(text));
}

static bool append_escaped(rtcp_plist_writer_t *writer, const char *text)
{
    for (const char *p = text; *p != '\0'; ++p) {
        const char *entity = NULL;
        switch (*p) {
            case '&':
                entity = "&amp;";
                break;
            case '<':
                entity = "&lt;";
                break;
            case '>':
                entity = "&gt;";
                break;
            case '"':
                entity = "&quot;";
                break;
            case '\'':
                entity = "&apos;";
                break;
            default:
                break;
        }
        if (entity != NULL) {
            if (!append_literal(writer, entity)) {
                return false;
            }
        } else if (!append_bytes(writer, p, 1U)) {
            return false;
        }
    }
    return true;
}

void rtcp_plist_writer_init(rtcp_plist_writer_t *writer, void *buffer,
                            size_t capacity)
{
    if (writer == NULL) {
        return;
    }
    memset(writer, 0, sizeof(*writer));
    writer->buffer = buffer;
    writer->capacity = capacity;
}

static bool can_write_value(const rtcp_plist_writer_t *writer)
{
    return !writer->failed && writer->depth != 0U &&
           writer->state[writer->depth - 1U] == 1U;
}

static void value_written(rtcp_plist_writer_t *writer)
{
    writer->state[writer->depth - 1U] = 0U;
    writer->pending_key = false;
}

bool rtcp_plist_begin_dict(rtcp_plist_writer_t *writer)
{
    if (writer == NULL || writer->failed) {
        return false;
    }
    if (writer->depth == 0U) {
        if (writer->started || writer->closed) {
            writer->failed = true;
            return false;
        }
        if (!append_literal(writer, PLIST_HEADER) ||
            !append_literal(writer, "<dict>")) {
            return false;
        }
        writer->started = true;
        writer->depth = 1U;
        writer->state[0] = 0U;
        return true;
    }
    if (!can_write_value(writer) || writer->depth >= RTCP_PLIST_MAX_DEPTH) {
        writer->failed = true;
        return false;
    }
    if (!append_literal(writer, "<dict>")) {
        return false;
    }
    writer->state[writer->depth - 1U] = 0U;
    writer->state[writer->depth] = 0U;
    writer->pending_key = false;
    ++writer->depth;
    return true;
}

bool rtcp_plist_end_dict(rtcp_plist_writer_t *writer)
{
    if (writer == NULL || writer->failed || writer->depth == 0U ||
        writer->state[writer->depth - 1U] != 0U) {
        if (writer != NULL) {
            writer->failed = true;
        }
        return false;
    }
    if (!append_literal(writer, "</dict>")) {
        return false;
    }
    --writer->depth;
    if (writer->depth == 0U) {
        writer->closed = true;
    } else {
        writer->state[writer->depth - 1U] = 0U;
    }
    return true;
}

bool rtcp_plist_key(rtcp_plist_writer_t *writer, const char *key)
{
    if (writer == NULL || key == NULL || writer->failed ||
        writer->depth == 0U || writer->state[writer->depth - 1U] != 0U) {
        if (writer != NULL) {
            writer->failed = true;
        }
        return false;
    }
    if (!append_literal(writer, "<key>") || !append_escaped(writer, key) ||
        !append_literal(writer, "</key>")) {
        return false;
    }
    writer->state[writer->depth - 1U] = 1U;
    writer->pending_key = true;
    return true;
}

bool rtcp_plist_string(rtcp_plist_writer_t *writer, const char *value)
{
    if (writer == NULL || value == NULL || !can_write_value(writer)) {
        return false;
    }
    if (!append_literal(writer, "<string>") ||
        !append_escaped(writer, value) ||
        !append_literal(writer, "</string>")) {
        return false;
    }
    value_written(writer);
    return true;
}

bool rtcp_plist_integer(rtcp_plist_writer_t *writer, int64_t value)
{
    char text[24];
    int length;
    if (writer == NULL || !can_write_value(writer)) {
        return false;
    }
    length = snprintf(text, sizeof(text), "%lld", (long long)value);
    if (length <= 0 || (size_t)length >= sizeof(text)) {
        writer->failed = true;
        return false;
    }
    if (!append_literal(writer, "<integer>") ||
        !append_bytes(writer, text, (size_t)length) ||
        !append_literal(writer, "</integer>")) {
        return false;
    }
    value_written(writer);
    return true;
}

bool rtcp_plist_boolean(rtcp_plist_writer_t *writer, bool value)
{
    if (writer == NULL || !can_write_value(writer)) {
        return false;
    }
    if (!append_literal(writer, value ? "<true/>" : "<false/>")) {
        return false;
    }
    value_written(writer);
    return true;
}

static bool append_base64(rtcp_plist_writer_t *writer, const uint8_t *data,
                          size_t length)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t offset = 0U;
    while (offset < length) {
        size_t remaining = length - offset;
        uint32_t group = (uint32_t)data[offset] << 16;
        if (remaining > 1U) {
            group |= (uint32_t)data[offset + 1U] << 8;
        }
        if (remaining > 2U) {
            group |= data[offset + 2U];
        }
        char out[4];
        out[0] = alphabet[(group >> 18) & 0x3fU];
        out[1] = alphabet[(group >> 12) & 0x3fU];
        out[2] = remaining > 1U ? alphabet[(group >> 6) & 0x3fU] : '=';
        out[3] = remaining > 2U ? alphabet[group & 0x3fU] : '=';
        if (!append_bytes(writer, out, sizeof(out))) {
            return false;
        }
        offset += remaining > 2U ? 3U : remaining;
    }
    return true;
}

bool rtcp_plist_data(rtcp_plist_writer_t *writer, const uint8_t *data,
                     size_t length)
{
    if (writer == NULL || !can_write_value(writer) ||
        (data == NULL && length != 0U)) {
        return false;
    }
    if (!append_literal(writer, "<data>") ||
        !append_base64(writer, data, length) ||
        !append_literal(writer, "</data>")) {
        return false;
    }
    value_written(writer);
    return true;
}

size_t rtcp_plist_finish(rtcp_plist_writer_t *writer)
{
    if (writer == NULL || writer->failed || !writer->started ||
        !writer->closed || writer->depth != 0U || writer->pending_key) {
        return 0U;
    }
    if (!append_literal(writer, PLIST_FOOTER)) {
        return 0U;
    }
    if (writer->length < writer->capacity) {
        writer->buffer[writer->length] = '\0';
    }
    return writer->length;
}

/* ------------------------------------------------------------------ */
/* Read-side helpers                                                   */
/* ------------------------------------------------------------------ */

static bool is_bare_key(const char *key)
{
    if (key == NULL || key[0] == '\0') {
        return false;
    }
    size_t length = 0U;
    for (const char *p = key; *p != '\0'; ++p) {
        char c = *p;
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                    c == '_';
        if (!safe || ++length > PLIST_KEY_MAX) {
            return false;
        }
    }
    return true;
}

static const uint8_t *find_bytes(const uint8_t *haystack, size_t haystack_len,
                                 const char *needle, size_t needle_len)
{
    if (needle_len == 0U || haystack_len < needle_len) {
        return NULL;
    }
    for (size_t i = 0U; i + needle_len <= haystack_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

/* Return the position of the value element that follows <key>key</key>. */
static int find_value(const uint8_t *xml, size_t length, const char *key,
                      const uint8_t **value)
{
    char needle[PLIST_KEY_MAX + 16U];
    int needle_length;
    if (xml == NULL || value == NULL || !is_bare_key(key)) {
        return RTCP_EINVAL;
    }
    needle_length = snprintf(needle, sizeof(needle), "<key>%s</key>", key);
    if (needle_length <= 0 || (size_t)needle_length >= sizeof(needle)) {
        return RTCP_EINVAL;
    }
    const uint8_t *position =
        find_bytes(xml, length, needle, (size_t)needle_length);
    if (position == NULL) {
        return RTCP_ENOENT;
    }
    size_t offset = (size_t)(position - xml) + (size_t)needle_length;
    while (offset < length) {
        uint8_t c = xml[offset];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        ++offset;
    }
    if (offset >= length || xml[offset] != '<') {
        return RTCP_EINVAL;
    }
    *value = xml + offset;
    return RTCP_OK;
}

static size_t bounded_remaining(const uint8_t *xml, size_t length,
                                const uint8_t *position)
{
    return length - (size_t)(position - xml);
}

static int decode_xml_text(const uint8_t *text, size_t text_length,
                           char *output, size_t capacity)
{
    size_t written = 0U;
    for (size_t i = 0U; i < text_length;) {
        if (text[i] == '&') {
            const char *replacement = NULL;
            size_t consumed = 0U;
            if (text_length - i >= 5U &&
                memcmp(text + i, "&amp;", 5U) == 0) {
                replacement = "&";
                consumed = 5U;
            } else if (text_length - i >= 4U &&
                       memcmp(text + i, "&lt;", 4U) == 0) {
                replacement = "<";
                consumed = 4U;
            } else if (text_length - i >= 4U &&
                       memcmp(text + i, "&gt;", 4U) == 0) {
                replacement = ">";
                consumed = 4U;
            } else if (text_length - i >= 6U &&
                       memcmp(text + i, "&quot;", 6U) == 0) {
                replacement = "\"";
                consumed = 6U;
            } else if (text_length - i >= 6U &&
                       memcmp(text + i, "&apos;", 6U) == 0) {
                replacement = "'";
                consumed = 6U;
            }
            if (replacement != NULL) {
                if (written + 1U >= capacity) {
                    return RTCP_EINVAL;
                }
                output[written++] = replacement[0];
                i += consumed;
                continue;
            }
        }
        if (written + 1U >= capacity) {
            return RTCP_EINVAL;
        }
        output[written++] = (char)text[i++];
    }
    if (output != NULL && capacity != 0U) {
        output[written] = '\0';
    }
    return RTCP_OK;
}

static int tag_bounds(const uint8_t *xml, size_t length, const uint8_t *value,
                      const char *open_tag, const char *close_tag,
                      const uint8_t **content, size_t *content_length)
{
    size_t remaining = bounded_remaining(xml, length, value);
    size_t open_length = strlen(open_tag);
    size_t close_length = strlen(close_tag);
    if (remaining < open_length ||
        memcmp(value, open_tag, open_length) != 0) {
        return RTCP_EINVAL;
    }
    const uint8_t *start = value + open_length;
    const uint8_t *close = find_bytes(start, bounded_remaining(xml, length,
                                                               start),
                                      close_tag, close_length);
    if (close == NULL) {
        return RTCP_EINVAL;
    }
    *content = start;
    *content_length = (size_t)(close - start);
    return RTCP_OK;
}

int rtcp_plist_get_string(const uint8_t *xml, size_t length, const char *key,
                          char *output, size_t capacity)
{
    const uint8_t *value = NULL;
    const uint8_t *content = NULL;
    size_t content_length = 0U;
    int ret = find_value(xml, length, key, &value);
    if (ret != RTCP_OK) {
        return ret;
    }
    if (output == NULL || capacity == 0U) {
        return RTCP_EINVAL;
    }
    ret = tag_bounds(xml, length, value, "<string>", "</string>", &content,
                     &content_length);
    if (ret != RTCP_OK) {
        return ret;
    }
    return decode_xml_text(content, content_length, output, capacity);
}

int rtcp_plist_get_integer(const uint8_t *xml, size_t length, const char *key,
                           int64_t *value)
{
    const uint8_t *element = NULL;
    const uint8_t *content = NULL;
    size_t content_length = 0U;
    int ret = find_value(xml, length, key, &element);
    if (ret != RTCP_OK) {
        return ret;
    }
    if (value == NULL) {
        return RTCP_EINVAL;
    }
    ret = tag_bounds(xml, length, element, "<integer>", "</integer>",
                     &content, &content_length);
    if (ret != RTCP_OK) {
        return ret;
    }
    if (content_length == 0U || content_length > 31U) {
        return RTCP_EINVAL;
    }
    char text[32];
    memcpy(text, content, content_length);
    text[content_length] = '\0';
    char *begin = text;
    while (*begin == ' ' || *begin == '\t' || *begin == '\r' ||
           *begin == '\n') {
        ++begin;
    }
    char *end = NULL;
    long long parsed = strtoll(begin, &end, 10);
    if (end == begin) {
        return RTCP_EINVAL;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }
    if (*end != '\0') {
        return RTCP_EINVAL;
    }
    *value = (int64_t)parsed;
    return RTCP_OK;
}

int rtcp_plist_get_boolean(const uint8_t *xml, size_t length, const char *key,
                           bool *value)
{
    const uint8_t *element = NULL;
    int ret = find_value(xml, length, key, &element);
    if (ret != RTCP_OK) {
        return ret;
    }
    if (value == NULL) {
        return RTCP_EINVAL;
    }
    size_t remaining = bounded_remaining(xml, length, element);
    if (remaining >= 7U && memcmp(element, "<true/>", 7U) == 0) {
        *value = true;
        return RTCP_OK;
    }
    if (remaining >= 8U && memcmp(element, "<false/>", 8U) == 0) {
        *value = false;
        return RTCP_OK;
    }
    return RTCP_EINVAL;
}

static int base64_value(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static int base64_decode(const uint8_t *input, size_t length, uint8_t *output,
                         size_t capacity, size_t *output_length)
{
    size_t significant = 0U;
    unsigned padding = 0U;
    for (size_t i = 0U; i < length; ++i) {
        uint8_t c = input[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        }
        if (c == '=') {
            ++padding;
        } else {
            if (padding != 0U || base64_value(c) < 0) {
                return RTCP_EINVAL;
            }
        }
        ++significant;
    }
    if (significant == 0U) {
        *output_length = 0U;
        return RTCP_OK;
    }
    if (significant % 4U != 0U || padding > 2U) {
        return RTCP_EINVAL;
    }

    size_t produced = (significant / 4U) * 3U - padding;
    if (produced > capacity || output == NULL) {
        return RTCP_EINVAL;
    }

    size_t written = 0U;
    unsigned group = 0U;
    unsigned index = 0U;
    bool group_padded = false;
    for (size_t i = 0U; i < length; ++i) {
        uint8_t c = input[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        }
        int digit = c == '=' ? 0 : base64_value(c);
        if (c == '=') {
            group_padded = true;
        } else if (group_padded) {
            return RTCP_EINVAL;
        }
        group = (group << 6) | (unsigned)digit;
        ++index;
        if (index == 4U) {
            if (written < produced) {
                output[written++] = (uint8_t)(group >> 16);
            }
            if (written < produced) {
                output[written++] = (uint8_t)(group >> 8);
            }
            if (written < produced) {
                output[written++] = (uint8_t)group;
            }
            group = 0U;
            index = 0U;
            group_padded = false;
        }
    }
    *output_length = written;
    return RTCP_OK;
}

int rtcp_plist_get_data(const uint8_t *xml, size_t length, const char *key,
                        uint8_t *output, size_t capacity,
                        size_t *output_length)
{
    const uint8_t *element = NULL;
    const uint8_t *content = NULL;
    size_t content_length = 0U;
    int ret = find_value(xml, length, key, &element);
    if (ret != RTCP_OK) {
        return ret;
    }
    if (output == NULL || output_length == NULL) {
        return RTCP_EINVAL;
    }
    ret = tag_bounds(xml, length, element, "<data>", "</data>", &content,
                     &content_length);
    if (ret != RTCP_OK) {
        return ret;
    }
    return base64_decode(content, content_length, output, capacity,
                         output_length);
}

int rtcp_plist_get_error(const uint8_t *xml, size_t length, char *output,
                         size_t capacity)
{
    int ret = rtcp_plist_get_string(xml, length, "Error", output, capacity);
    if (ret == RTCP_OK) {
        return RTCP_OK;
    }
    if (ret == RTCP_ENOENT) {
        return RTCP_ENOENT;
    }

    int64_t code = 0;
    if (rtcp_plist_get_integer(xml, length, "Error", &code) != RTCP_OK) {
        return RTCP_EINVAL;
    }
    if (rtcp_plist_get_string(xml, length, "ErrorString", output,
                              capacity) == RTCP_OK) {
        return RTCP_OK;
    }
    int written = snprintf(output, capacity, "%lld", (long long)code);
    if (written <= 0 || (size_t)written >= capacity) {
        return RTCP_EINVAL;
    }
    return RTCP_OK;
}
