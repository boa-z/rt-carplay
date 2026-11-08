/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bounded RTSP event-stream parsing follows DiPlay AirPlaySession.kt. This
 * component leaves socket transport and encryption to the caller.
 */
#include "airplay_control_stream.h"

#include <stdlib.h>
#include <string.h>
#include "airplay_control_memory.h"

struct airplay_control_stream {
    uint8_t buffer[AIRPLAY_CONTROL_STREAM_MAX_MESSAGE_BYTES];
    size_t buffered;
    airplay_rtsp_request_t request;
};

static bool find_header_end(const uint8_t *buffer, size_t len, size_t *end)
{
    if (buffer == NULL || end == NULL || len < 4U) return false;
    for (size_t i = 0U; i <= len - 4U; ++i) {
        if (buffer[i] == '\r' && buffer[i + 1U] == '\n' &&
            buffer[i + 2U] == '\r' && buffer[i + 3U] == '\n') {
            *end = i;
            return true;
        }
    }
    return false;
}

static bool ascii_equal_ci(const uint8_t *left, size_t left_len,
                           const char *right)
{
    size_t right_len = strlen(right);
    if (left_len != right_len) return false;
    for (size_t i = 0U; i < left_len; ++i) {
        uint8_t a = left[i];
        uint8_t b = (uint8_t)right[i];
        if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (uint8_t)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

static airplay_control_stream_result_t parse_response_body_len(
    const uint8_t *buffer,
    size_t header_end,
    size_t *body_len)
{
    size_t line_end = 0U;
    while (line_end < header_end &&
           !(buffer[line_end] == '\r' && buffer[line_end + 1U] == '\n')) {
        ++line_end;
    }
    if (line_end > header_end || buffer[line_end] != '\r' ||
        buffer[line_end + 1U] != '\n') {
        return AIRPLAY_CONTROL_STREAM_INVALID_MESSAGE;
    }

    size_t offset = line_end + 2U;
    bool saw_length = false;
    *body_len = 0U;
    while (offset < header_end) {
        size_t end = offset;
        while (end < header_end && buffer[end] != '\r') ++end;
        if (end > header_end || buffer[end] != '\r' || buffer[end + 1U] != '\n') {
            return AIRPLAY_CONTROL_STREAM_INVALID_MESSAGE;
        }
        size_t colon = offset;
        while (colon < end && buffer[colon] != ':') ++colon;
        if (colon == end) return AIRPLAY_CONTROL_STREAM_INVALID_MESSAGE;
        if (ascii_equal_ci(buffer + offset, colon - offset, "content-length")) {
            if (saw_length) return AIRPLAY_CONTROL_STREAM_INVALID_MESSAGE;
            saw_length = true;
            size_t value = colon + 1U;
            while (value < end && (buffer[value] == ' ' || buffer[value] == '\t')) ++value;
            if (value == end) return AIRPLAY_CONTROL_STREAM_INVALID_MESSAGE;
            size_t parsed = 0U;
            for (; value < end; ++value) {
                if (buffer[value] < '0' || buffer[value] > '9') {
                    return AIRPLAY_CONTROL_STREAM_INVALID_MESSAGE;
                }
                size_t digit = (size_t)(buffer[value] - '0');
                if (parsed > (SIZE_MAX - digit) / 10U) {
                    return AIRPLAY_CONTROL_STREAM_TOO_LARGE;
                }
                parsed = parsed * 10U + digit;
            }
            if (parsed > AIRPLAY_CONTROL_STREAM_MAX_BODY_BYTES) {
                return AIRPLAY_CONTROL_STREAM_TOO_LARGE;
            }
            *body_len = parsed;
        }
        offset = end + 2U;
    }
    return AIRPLAY_CONTROL_STREAM_OK;
}

static bool is_response(const uint8_t *buffer, size_t len)
{
    return len >= 5U && (memcmp(buffer, "RTSP/", 5U) == 0 ||
                         memcmp(buffer, "HTTP/", 5U) == 0);
}

static void consume(airplay_control_stream_t *stream, size_t len)
{
    if (len < stream->buffered) {
        memmove(stream->buffer, stream->buffer + len, stream->buffered - len);
    }
    stream->buffered -= len;
}

airplay_control_stream_result_t airplay_control_stream_create(
    airplay_control_stream_t **out_stream)
{
    if (out_stream == NULL) return AIRPLAY_CONTROL_STREAM_INVALID_ARGUMENT;
    *out_stream = airplay_control_calloc(1U, sizeof(**out_stream));
    return *out_stream == NULL ? AIRPLAY_CONTROL_STREAM_NO_MEMORY
                               : AIRPLAY_CONTROL_STREAM_OK;
}

airplay_control_stream_result_t airplay_control_stream_feed(
    airplay_control_stream_t *stream,
    const uint8_t *plaintext,
    size_t plaintext_len,
    airplay_control_stream_request_cb_t on_request,
    void *context)
{
    if (stream == NULL || (plaintext == NULL && plaintext_len != 0U) ||
        on_request == NULL) {
        return AIRPLAY_CONTROL_STREAM_INVALID_ARGUMENT;
    }
    if (plaintext_len > sizeof(stream->buffer) - stream->buffered) {
        return AIRPLAY_CONTROL_STREAM_TOO_LARGE;
    }
    if (plaintext_len != 0U) {
        memcpy(stream->buffer + stream->buffered, plaintext, plaintext_len);
        stream->buffered += plaintext_len;
    }

    while (stream->buffered != 0U) {
        size_t header_end = 0U;
        if (!find_header_end(stream->buffer, stream->buffered, &header_end)) {
            return stream->buffered > AIRPLAY_RTSP_MAX_HEADER_BYTES
                       ? AIRPLAY_CONTROL_STREAM_TOO_LARGE
                       : AIRPLAY_CONTROL_STREAM_INCOMPLETE;
        }
        if (header_end + 4U > AIRPLAY_RTSP_MAX_HEADER_BYTES) {
            return AIRPLAY_CONTROL_STREAM_TOO_LARGE;
        }
        size_t consumed = 0U;
        if (is_response(stream->buffer, stream->buffered)) {
            size_t body_len = 0U;
            airplay_control_stream_result_t parsed = parse_response_body_len(
                stream->buffer, header_end, &body_len);
            if (parsed != AIRPLAY_CONTROL_STREAM_OK) return parsed;
            consumed = header_end + 4U + body_len;
            if (stream->buffered < consumed) {
                return AIRPLAY_CONTROL_STREAM_INCOMPLETE;
            }
        } else {
            airplay_rtsp_parse_result_t parsed = airplay_rtsp_parse_request(
                stream->buffer, stream->buffered, &stream->request);
            if (parsed == AIRPLAY_RTSP_PARSE_INCOMPLETE) {
                return AIRPLAY_CONTROL_STREAM_INCOMPLETE;
            }
            if (parsed == AIRPLAY_RTSP_PARSE_TOO_LARGE) {
                return AIRPLAY_CONTROL_STREAM_TOO_LARGE;
            }
            if (parsed != AIRPLAY_RTSP_PARSE_COMPLETE ||
                stream->request.body_len > AIRPLAY_CONTROL_STREAM_MAX_BODY_BYTES) {
                return AIRPLAY_CONTROL_STREAM_INVALID_MESSAGE;
            }
            if (!on_request(&stream->request, context)) {
                return AIRPLAY_CONTROL_STREAM_CALLBACK_FAILED;
            }
            consumed = stream->request.consumed;
            memset(&stream->request, 0, sizeof(stream->request));
        }
        if (consumed == 0U || consumed > stream->buffered) {
            return AIRPLAY_CONTROL_STREAM_INVALID_MESSAGE;
        }
        consume(stream, consumed);
    }
    return AIRPLAY_CONTROL_STREAM_OK;
}

void airplay_control_stream_destroy(airplay_control_stream_t *stream)
{
    if (stream == NULL) return;
    memset(stream, 0, sizeof(*stream));
    free(stream);
}
