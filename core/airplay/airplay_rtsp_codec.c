/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 ESP32 CarPlay Web Relay contributors
 * Bounded RTSP framing follows DiPlay's RtspMessage.kt. See THIRD_PARTY_NOTICES.md.
 */
#include "airplay_rtsp_codec.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool token_char(uint8_t value)
{
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '_' ||
           value == '.' || value == '*' || value == '$' || value == '+' ||
           value == '%' || value == '!' || value == '\'' || value == '~';
}

static bool find_header_end(const uint8_t *buffer, size_t len, size_t *out_end)
{
    if (buffer == NULL || out_end == NULL || len < 4U) return false;
    for (size_t i = 0; i <= len - 4U; ++i) {
        if (buffer[i] == '\r' && buffer[i + 1U] == '\n' &&
            buffer[i + 2U] == '\r' && buffer[i + 3U] == '\n') {
            *out_end = i;
            return true;
        }
    }
    return false;
}

static bool copy_token(char *out, size_t out_capacity,
                       const uint8_t *bytes, size_t len, bool lowercase)
{
    if (out == NULL || bytes == NULL || len == 0 || len >= out_capacity) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!token_char(bytes[i])) return false;
        out[i] = lowercase && bytes[i] >= 'A' && bytes[i] <= 'Z'
                     ? (char)(bytes[i] + ('a' - 'A')) : (char)bytes[i];
    }
    out[len] = '\0';
    return true;
}

static bool parse_decimal(const uint8_t *bytes, size_t len, size_t *out_value)
{
    if (bytes == NULL || out_value == NULL || len == 0) return false;
    size_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        if (bytes[i] < '0' || bytes[i] > '9') return false;
        size_t digit = (size_t)(bytes[i] - '0');
        if (value > (SIZE_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    *out_value = value;
    return true;
}

static bool copy_header_value(char *out, size_t capacity,
                              const uint8_t *bytes, size_t len)
{
    while (len != 0 && (*bytes == ' ' || *bytes == '\t')) {
        ++bytes;
        --len;
    }
    while (len != 0 && (bytes[len - 1U] == ' ' || bytes[len - 1U] == '\t')) --len;
    if (len >= capacity) return false;
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = bytes[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
        out[i] = (char)c;
    }
    out[len] = '\0';
    return true;
}

airplay_rtsp_parse_result_t airplay_rtsp_parse_request(const uint8_t *buffer,
                                                       size_t buffer_len,
                                                       airplay_rtsp_request_t *request)
{
    if (buffer == NULL || request == NULL) return AIRPLAY_RTSP_PARSE_INVALID;
    memset(request, 0, sizeof(*request));

    size_t header_end = 0;
    if (!find_header_end(buffer, buffer_len, &header_end)) {
        return buffer_len > AIRPLAY_RTSP_MAX_HEADER_BYTES
                   ? AIRPLAY_RTSP_PARSE_TOO_LARGE : AIRPLAY_RTSP_PARSE_INCOMPLETE;
    }
    if (header_end + 4U > AIRPLAY_RTSP_MAX_HEADER_BYTES) return AIRPLAY_RTSP_PARSE_TOO_LARGE;

    size_t line_end = 0;
    while (line_end + 1U < header_end &&
           !(buffer[line_end] == '\r' && buffer[line_end + 1U] == '\n')) ++line_end;
    if (line_end + 1U >= header_end || line_end == 0) return AIRPLAY_RTSP_PARSE_INVALID;

    size_t first_space = 0;
    while (first_space < line_end && buffer[first_space] != ' ') ++first_space;
    size_t second_space = first_space + 1U;
    while (second_space < line_end && buffer[second_space] != ' ') ++second_space;
    if (first_space == 0 || first_space >= line_end || second_space <= first_space + 1U ||
        second_space + 1U >= line_end ||
        !copy_token(request->method, sizeof(request->method), buffer, first_space, false)) {
        return AIRPLAY_RTSP_PARSE_INVALID;
    }
    size_t path_len = second_space - first_space - 1U;
    if (path_len >= sizeof(request->path)) return AIRPLAY_RTSP_PARSE_TOO_LARGE;
    for (size_t i = 0; i < path_len; ++i) {
        uint8_t c = buffer[first_space + 1U + i];
        if (c <= 0x20 || c == 0x7f) return AIRPLAY_RTSP_PARSE_INVALID;
        request->path[i] = (char)c;
    }
    request->path[path_len] = '\0';
    size_t protocol_len = line_end - second_space - 1U;
    if (protocol_len >= sizeof(request->protocol) || protocol_len == 0) {
        return AIRPLAY_RTSP_PARSE_INVALID;
    }
    if (protocol_len != sizeof("RTSP/1.0") - 1U ||
        memcmp(buffer + second_space + 1U, "RTSP/1.0", sizeof("RTSP/1.0") - 1U) != 0) {
        return AIRPLAY_RTSP_PARSE_INVALID;
    }
    memcpy(request->protocol, buffer + second_space + 1U, protocol_len);
    request->protocol[protocol_len] = '\0';

    bool have_content_length = false;
    size_t content_length = 0;
    size_t offset = line_end + 2U;
    size_t header_lines_end = header_end + 2U;
    while (offset < header_end) {
        size_t end = offset;
        while (end + 1U < header_lines_end &&
               !(buffer[end] == '\r' && buffer[end + 1U] == '\n')) ++end;
        if (end + 1U >= header_lines_end || end == offset) return AIRPLAY_RTSP_PARSE_INVALID;
        size_t colon = offset;
        while (colon < end && buffer[colon] != ':') ++colon;
        if (colon == offset || colon == end || request->header_count >= AIRPLAY_RTSP_MAX_HEADERS) {
            return AIRPLAY_RTSP_PARSE_INVALID;
        }
        airplay_rtsp_header_t *header = &request->headers[request->header_count];
        if (!copy_token(header->name, sizeof(header->name), buffer + offset, colon - offset, true) ||
            !copy_header_value(header->value, sizeof(header->value),
                               buffer + colon + 1U, end - colon - 1U)) {
            return AIRPLAY_RTSP_PARSE_INVALID;
        }
        for (size_t i = 0; i < request->header_count; ++i) {
            if (strcmp(request->headers[i].name, header->name) == 0) return AIRPLAY_RTSP_PARSE_INVALID;
        }
        if (strcmp(header->name, "content-length") == 0) {
            if (!parse_decimal((const uint8_t *)header->value, strlen(header->value), &content_length)) {
                return AIRPLAY_RTSP_PARSE_INVALID;
            }
            if (content_length > AIRPLAY_RTSP_MAX_BODY_BYTES) return AIRPLAY_RTSP_PARSE_TOO_LARGE;
            have_content_length = true;
        } else if (strcmp(header->name, "cseq") == 0) {
            size_t value_len = strlen(header->value);
            if (value_len == 0 || value_len >= sizeof(request->cseq)) return AIRPLAY_RTSP_PARSE_INVALID;
            for (size_t i = 0; i < value_len; ++i) {
                if (header->value[i] < '0' || header->value[i] > '9') return AIRPLAY_RTSP_PARSE_INVALID;
            }
            memcpy(request->cseq, header->value, value_len + 1U);
        }
        ++request->header_count;
        offset = end + 2U;
    }

    if (request->cseq[0] == '\0') return AIRPLAY_RTSP_PARSE_INVALID;
    if (!have_content_length) content_length = 0;
    size_t body_start = header_end + 4U;
    if (content_length > buffer_len - body_start) return AIRPLAY_RTSP_PARSE_INCOMPLETE;
    request->body = buffer + body_start;
    request->body_len = content_length;
    request->consumed = body_start + content_length;
    return AIRPLAY_RTSP_PARSE_COMPLETE;
}

static bool append_bytes(uint8_t *output, size_t capacity, size_t *used,
                         const void *bytes, size_t len)
{
    if (*used > capacity || len > capacity - *used) return false;
    if (len == 0) return true;
    memmove(output + *used, bytes, len);
    *used += len;
    return true;
}

static bool append_text(uint8_t *output, size_t capacity, size_t *used, const char *text)
{
    return text != NULL && append_bytes(output, capacity, used, text, strlen(text));
}

static bool valid_cseq(const char *cseq, size_t capacity)
{
    if (cseq == NULL || capacity == 0 || cseq[0] == '\0') return false;
    size_t len = 0;
    while (len < capacity && cseq[len] != '\0') {
        if (cseq[len] < '0' || cseq[len] > '9') return false;
        ++len;
    }
    return len != 0 && len < capacity;
}

int airplay_rtsp_build_response(const airplay_rtsp_request_t *request,
                                unsigned status,
                                const char *reason,
                                const char *content_type,
                                const uint8_t *body,
                                size_t body_len,
                                uint8_t *output,
                                size_t output_capacity,
                                size_t *output_len)
{
    if (output_len != NULL) *output_len = 0;
    if (request == NULL || reason == NULL || output == NULL || output_len == NULL ||
        (body == NULL && body_len != 0) || body_len > AIRPLAY_RTSP_MAX_BODY_BYTES ||
        status < 100U || status > 999U ||
        memcmp(request->protocol, "RTSP/1.0", sizeof("RTSP/1.0")) != 0 ||
        !valid_cseq(request->cseq, sizeof(request->cseq))) return -1;
    for (const char *p = reason; *p != '\0'; ++p) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f) return -1;
    }
    if (content_type != NULL) {
        for (const char *p = content_type; *p != '\0'; ++p) {
            if ((unsigned char)*p < 0x20 || *p == 0x7f || *p == '\r' || *p == '\n') return -1;
        }
    }

    size_t used = 0;
    char line[96];
    int line_len = snprintf(line, sizeof(line), "%s %u %s\r\n",
                            request->protocol, status, reason);
    if (line_len <= 0 || (size_t)line_len >= sizeof(line) ||
        !append_bytes(output, output_capacity, &used, line, (size_t)line_len)) return -1;
    if (request->cseq[0] != '\0') {
        if (!append_text(output, output_capacity, &used, "CSeq: ") ||
            !append_text(output, output_capacity, &used, request->cseq) ||
            !append_text(output, output_capacity, &used, "\r\n")) return -1;
    }
    if (content_type != NULL && content_type[0] != '\0') {
        if (!append_text(output, output_capacity, &used, "Content-Type: ") ||
            !append_text(output, output_capacity, &used, content_type) ||
            !append_text(output, output_capacity, &used, "\r\n")) return -1;
    }
    line_len = snprintf(line, sizeof(line), "Content-Length: %u\r\n\r\n", (unsigned)body_len);
    if (line_len <= 0 || (size_t)line_len >= sizeof(line) ||
        !append_bytes(output, output_capacity, &used, line, (size_t)line_len) ||
        !append_bytes(output, output_capacity, &used, body, body_len)) return -1;
    *output_len = used;
    return 0;
}
