/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bounded RTSP framing follows DiPlay's RtspMessage.kt. See THIRD_PARTY_NOTICES.md.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_RTSP_METHOD_BYTES 16U
#define AIRPLAY_RTSP_PATH_BYTES 160U
#define AIRPLAY_RTSP_CSEQ_BYTES 16U
#define AIRPLAY_RTSP_HEADER_NAME_BYTES 32U
#define AIRPLAY_RTSP_HEADER_VALUE_BYTES 192U
#define AIRPLAY_RTSP_MAX_HEADERS 24U
#define AIRPLAY_RTSP_MAX_HEADER_BYTES 2048U
#define AIRPLAY_RTSP_MAX_BODY_BYTES (32U * 1024U)

typedef enum {
    AIRPLAY_RTSP_PARSE_INVALID = -1,
    AIRPLAY_RTSP_PARSE_TOO_LARGE = -2,
    AIRPLAY_RTSP_PARSE_INCOMPLETE = 0,
    AIRPLAY_RTSP_PARSE_COMPLETE = 1,
} airplay_rtsp_parse_result_t;

typedef struct {
    char name[AIRPLAY_RTSP_HEADER_NAME_BYTES]; /* lowercase */
    char value[AIRPLAY_RTSP_HEADER_VALUE_BYTES];
} airplay_rtsp_header_t;

typedef struct {
    char method[AIRPLAY_RTSP_METHOD_BYTES];
    char path[AIRPLAY_RTSP_PATH_BYTES];
    char protocol[AIRPLAY_RTSP_METHOD_BYTES];
    char cseq[AIRPLAY_RTSP_CSEQ_BYTES];
    airplay_rtsp_header_t headers[AIRPLAY_RTSP_MAX_HEADERS];
    size_t header_count;
    const uint8_t *body; /* view into the caller's receive buffer */
    size_t body_len;
    size_t consumed;
} airplay_rtsp_request_t;

/* Parse one complete request from a stream buffer. INCOMPLETE preserves the
 * caller's bytes; COMPLETE reports exact consumption so pipelined bytes remain. */
airplay_rtsp_parse_result_t airplay_rtsp_parse_request(const uint8_t *buffer,
                                                       size_t buffer_len,
                                                       airplay_rtsp_request_t *request);

/* Build a bounded response, echoing the request protocol and CSeq. */
int airplay_rtsp_build_response(const airplay_rtsp_request_t *request,
                                unsigned status,
                                const char *reason,
                                const char *content_type,
                                const uint8_t *body,
                                size_t body_len,
                                uint8_t *output,
                                size_t output_capacity,
                                size_t *output_len);
