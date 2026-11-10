/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lockdown.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

#define LOCKDOWN_PLIST_HEADER \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" \
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" " \
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n" \
    "<plist version=\"1.0\">\n" \
    "<dict>\n"

#define LOCKDOWN_PLIST_FOOTER \
    "</dict>\n" \
    "</plist>\n"

static bool is_bare_token(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (const char *p = value; *p != '\0'; ++p) {
        char c = *p;
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                    c == '_';
        if (!safe) {
            return false;
        }
    }
    return true;
}

size_t rtcp_lockdown_build_get_value(uint8_t *output, size_t capacity,
                                     const char *label, const char *key)
{
    if (output == NULL || !is_bare_token(label) || !is_bare_token(key)) {
        return 0U;
    }
    int written = snprintf(
        (char *)output, capacity,
        LOCKDOWN_PLIST_HEADER
        "<key>Label</key><string>%s</string>\n"
        "<key>Request</key><string>GetValue</string>\n"
        "<key>Key</key><string>%s</string>\n" LOCKDOWN_PLIST_FOOTER,
        label, key);
    if (written < 0 || (size_t)written >= capacity) {
        return 0U;
    }
    return (size_t)written;
}

/* Bounded byte search: the XML buffer is not NUL terminated. */
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

int rtcp_lockdown_extract_string(const uint8_t *xml, size_t length,
                                 const char *key, char *output,
                                 size_t capacity)
{
    static const char string_open[] = "<string>";
    static const char string_close[] = "</string>";
    if (xml == NULL || output == NULL || capacity == 0U ||
        !is_bare_token(key)) {
        return RTCP_EINVAL;
    }

    char needle[64];
    int needle_len = snprintf(needle, sizeof(needle), "<key>%s</key>", key);
    if (needle_len < 0 || (size_t)needle_len >= sizeof(needle)) {
        return RTCP_EINVAL;
    }
    const uint8_t *position =
        find_bytes(xml, length, needle, (size_t)needle_len);
    if (position == NULL) {
        return RTCP_ENOENT;
    }

    size_t remaining = length - (size_t)(position - xml);
    const uint8_t *open = find_bytes(position, remaining, string_open,
                                     sizeof(string_open) - 1U);
    if (open == NULL) {
        return RTCP_EINVAL;
    }
    open += sizeof(string_open) - 1U;
    remaining = length - (size_t)(open - xml);
    const uint8_t *close = find_bytes(open, remaining, string_close,
                                      sizeof(string_close) - 1U);
    if (close == NULL) {
        return RTCP_EINVAL;
    }
    size_t value_length = (size_t)(close - open);
    if (value_length >= capacity) {
        return RTCP_EINVAL;
    }
    memcpy(output, open, value_length);
    output[value_length] = '\0';
    return RTCP_OK;
}
