/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lockdown.h"

#include <stdbool.h>
#include <string.h>

#include "rtcp/plist.h"
#include "rtcp/rtcp.h"

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

static bool writer_usable(const uint8_t *output, size_t capacity)
{
    return output != NULL && capacity != 0U;
}

static size_t finish(rtcp_plist_writer_t *writer)
{
    return rtcp_plist_finish(writer);
}

size_t rtcp_lockdown_build_get_value(uint8_t *output, size_t capacity,
                                     const char *label, const char *key)
{
    if (!writer_usable(output, capacity) || !is_bare_token(label) ||
        !is_bare_token(key)) {
        return 0U;
    }
    rtcp_plist_writer_t writer;
    rtcp_plist_writer_init(&writer, output, capacity);
    if (!rtcp_plist_begin_dict(&writer) ||
        !rtcp_plist_key(&writer, "Label") ||
        !rtcp_plist_string(&writer, label) ||
        !rtcp_plist_key(&writer, "Request") ||
        !rtcp_plist_string(&writer, "GetValue") ||
        !rtcp_plist_key(&writer, "Key") ||
        !rtcp_plist_string(&writer, key) ||
        !rtcp_plist_end_dict(&writer)) {
        return 0U;
    }
    return finish(&writer);
}

size_t rtcp_lockdown_build_set_value(uint8_t *output, size_t capacity,
                                     const char *label, const char *key,
                                     const char *value)
{
    if (!writer_usable(output, capacity) || !is_bare_token(label) ||
        !is_bare_token(key) || value == NULL) {
        return 0U;
    }
    rtcp_plist_writer_t writer;
    rtcp_plist_writer_init(&writer, output, capacity);
    if (!rtcp_plist_begin_dict(&writer) ||
        !rtcp_plist_key(&writer, "Label") ||
        !rtcp_plist_string(&writer, label) ||
        !rtcp_plist_key(&writer, "Request") ||
        !rtcp_plist_string(&writer, "SetValue") ||
        !rtcp_plist_key(&writer, "Key") ||
        !rtcp_plist_string(&writer, key) ||
        !rtcp_plist_key(&writer, "Value") ||
        !rtcp_plist_string(&writer, value) ||
        !rtcp_plist_end_dict(&writer)) {
        return 0U;
    }
    return finish(&writer);
}

size_t rtcp_lockdown_build_start_session(uint8_t *output, size_t capacity,
                                         const char *label,
                                         const char *host_id,
                                         const char *system_buid)
{
    if (!writer_usable(output, capacity) || !is_bare_token(label) ||
        !is_bare_token(host_id) || !is_bare_token(system_buid)) {
        return 0U;
    }
    rtcp_plist_writer_t writer;
    rtcp_plist_writer_init(&writer, output, capacity);
    if (!rtcp_plist_begin_dict(&writer) ||
        !rtcp_plist_key(&writer, "Label") ||
        !rtcp_plist_string(&writer, label) ||
        !rtcp_plist_key(&writer, "Request") ||
        !rtcp_plist_string(&writer, "StartSession") ||
        !rtcp_plist_key(&writer, "HostID") ||
        !rtcp_plist_string(&writer, host_id) ||
        !rtcp_plist_key(&writer, "SystemBUID") ||
        !rtcp_plist_string(&writer, system_buid) ||
        !rtcp_plist_end_dict(&writer)) {
        return 0U;
    }
    return finish(&writer);
}

size_t rtcp_lockdown_build_start_service(uint8_t *output, size_t capacity,
                                         const char *service)
{
    if (!writer_usable(output, capacity) || !is_bare_token(service)) {
        return 0U;
    }
    rtcp_plist_writer_t writer;
    rtcp_plist_writer_init(&writer, output, capacity);
    if (!rtcp_plist_begin_dict(&writer) ||
        !rtcp_plist_key(&writer, "Request") ||
        !rtcp_plist_string(&writer, "StartService") ||
        !rtcp_plist_key(&writer, "Service") ||
        !rtcp_plist_string(&writer, service) ||
        !rtcp_plist_end_dict(&writer)) {
        return 0U;
    }
    return finish(&writer);
}

size_t rtcp_lockdown_build_pair(
    uint8_t *output, size_t capacity, const char *label, const char *host_id,
    const char *system_buid, const uint8_t *device_certificate,
    size_t device_certificate_length, const uint8_t *host_certificate,
    size_t host_certificate_length, const uint8_t *root_certificate,
    size_t root_certificate_length)
{
    if (!writer_usable(output, capacity) || !is_bare_token(label) ||
        !is_bare_token(host_id) || !is_bare_token(system_buid) ||
        device_certificate == NULL || device_certificate_length == 0U ||
        host_certificate == NULL || host_certificate_length == 0U ||
        root_certificate == NULL || root_certificate_length == 0U) {
        return 0U;
    }
    rtcp_plist_writer_t writer;
    rtcp_plist_writer_init(&writer, output, capacity);
    if (!rtcp_plist_begin_dict(&writer) ||
        !rtcp_plist_key(&writer, "Label") ||
        !rtcp_plist_string(&writer, label) ||
        !rtcp_plist_key(&writer, "PairRecord") ||
        !rtcp_plist_begin_dict(&writer) ||
        !rtcp_plist_key(&writer, "DeviceCertificate") ||
        !rtcp_plist_data(&writer, device_certificate,
                         device_certificate_length) ||
        !rtcp_plist_key(&writer, "HostCertificate") ||
        !rtcp_plist_data(&writer, host_certificate,
                         host_certificate_length) ||
        !rtcp_plist_key(&writer, "HostID") ||
        !rtcp_plist_string(&writer, host_id) ||
        !rtcp_plist_key(&writer, "RootCertificate") ||
        !rtcp_plist_data(&writer, root_certificate,
                         root_certificate_length) ||
        !rtcp_plist_key(&writer, "SystemBUID") ||
        !rtcp_plist_string(&writer, system_buid) ||
        !rtcp_plist_end_dict(&writer) ||
        !rtcp_plist_key(&writer, "Request") ||
        !rtcp_plist_string(&writer, "Pair") ||
        !rtcp_plist_key(&writer, "ProtocolVersion") ||
        !rtcp_plist_string(&writer, "2") ||
        !rtcp_plist_key(&writer, "PairingOptions") ||
        !rtcp_plist_begin_dict(&writer) ||
        !rtcp_plist_key(&writer, "ExtendedPairingErrors") ||
        !rtcp_plist_boolean(&writer, true) ||
        !rtcp_plist_end_dict(&writer) ||
        !rtcp_plist_end_dict(&writer)) {
        return 0U;
    }
    return finish(&writer);
}

static int read_exact(const rtcp_stream_t *stream, uint8_t *buffer,
                      size_t length, uint32_t timeout_ms)
{
    size_t received = 0U;
    while (received < length) {
        int ret = stream->ops->read(stream->context, buffer + received,
                                    length - received, timeout_ms);
        if (ret < 0) {
            return ret;
        }
        if (ret == 0) {
            return RTCP_ESTATE;
        }
        received += (size_t)ret;
    }
    return RTCP_OK;
}

int rtcp_lockdown_exchange(const rtcp_stream_t *stream,
                           const uint8_t *request, size_t request_length,
                           uint8_t *reply, size_t reply_capacity,
                           size_t *reply_length, uint32_t timeout_ms)
{
    if (stream == NULL || stream->ops == NULL || stream->ops->read == NULL ||
        stream->ops->write == NULL || request == NULL || request_length == 0U ||
        request_length > RTCP_LOCKDOWN_FRAME_MAX || reply == NULL ||
        reply_capacity == 0U || reply_length == NULL) {
        return RTCP_EINVAL;
    }

    uint8_t header[4] = {
        (uint8_t)(request_length >> 24),
        (uint8_t)(request_length >> 16),
        (uint8_t)(request_length >> 8),
        (uint8_t)request_length,
    };
    int ret = stream->ops->write(stream->context, header, sizeof(header));
    if (ret < 0) {
        return ret;
    }
    ret = stream->ops->write(stream->context, request, request_length);
    if (ret < 0) {
        return ret;
    }

    ret = read_exact(stream, header, sizeof(header), timeout_ms);
    if (ret != RTCP_OK) {
        return ret;
    }
    size_t length = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) |
                    ((size_t)header[2] << 8) | (size_t)header[3];
    if (length == 0U || length > reply_capacity ||
        length > RTCP_LOCKDOWN_FRAME_MAX) {
        return RTCP_EINVAL;
    }
    ret = read_exact(stream, reply, length, timeout_ms);
    if (ret != RTCP_OK) {
        return ret;
    }
    *reply_length = length;
    return RTCP_OK;
}

int rtcp_lockdown_extract_string(const uint8_t *xml, size_t length,
                                 const char *key, char *output,
                                 size_t capacity)
{
    return rtcp_plist_get_string(xml, length, key, output, capacity);
}
