/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Event RTSP and HID command framing follows DiPlay's AirPlaySession.kt.
 * See NOTICE for attribution.  The ESP-IDF singleton was replaced by an
 * instance that takes its mutex from the OS seam.
 */
#include "airplay_control_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airplay_command_plist.h"
#include "airplay_bplist.h"
#include "airplay_control_stream.h"
#include "airplay_hid_plist.h"
#include "mbedtls/platform_util.h"
#include "rtcp/log.h"
#include "rtcp/rtcp.h"

#define TAG "airplay_control"

#define AIRPLAY_CONTROL_MAX_PLAINTEXT 512U
#define AIRPLAY_CONTROL_RESPONSE_BYTES 256U
#define AIRPLAY_KEYFRAME_REQUEST_INTERVAL_MS 250U
#define AIRPLAY_KEYFRAME_LOG_INTERVAL_MS 5000U

struct airplay_control_port {
    const struct rtcp_os *os;
    rtcp_mutex_t mutex;

    uint32_t stream_id;
    uint32_t cseq;
    airplay_control_write_cb_t write_cb;
    void *context;

    airplay_control_cipher_t *cipher;
    airplay_control_stream_t *stream;
    uint8_t *plaintext_scratch;
    bool active;

    bool keyframe_tick_valid;
    uint64_t keyframe_tick_ms;
    bool keyframe_log_tick_valid;
    uint64_t keyframe_log_tick_ms;
};

static void destroy_channel(airplay_control_port_t *port)
{
    airplay_control_cipher_destroy(port->cipher);
    airplay_control_stream_destroy(port->stream);
    port->cipher = NULL;
    port->stream = NULL;
    if (port->plaintext_scratch != NULL) {
        mbedtls_platform_zeroize(port->plaintext_scratch,
                                 AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES);
        free(port->plaintext_scratch);
        port->plaintext_scratch = NULL;
    }
    port->active = false;
}

static void lock(airplay_control_port_t *port)
{
    if (port->os->mutex_lock != NULL) port->os->mutex_lock(port->mutex);
}

static void unlock(airplay_control_port_t *port)
{
    if (port->os->mutex_unlock != NULL) port->os->mutex_unlock(port->mutex);
}

int airplay_control_port_create(const struct rtcp_os *os,
                                const uint8_t read_key[AIRPLAY_CONTROL_KEY_BYTES],
                                const uint8_t write_key[AIRPLAY_CONTROL_KEY_BYTES],
                                uint32_t stream_id,
                                uint32_t initial_cseq,
                                airplay_control_write_cb_t write_cb,
                                void *context,
                                airplay_control_port_t **out_port)
{
    if (out_port == NULL) return RTCP_EINVAL;
    *out_port = NULL;
    if (os == NULL || read_key == NULL || write_key == NULL ||
        write_cb == NULL) {
        return RTCP_EINVAL;
    }

    airplay_control_port_t *port = calloc(1U, sizeof(*port));
    if (port == NULL) return RTCP_ENOMEM;
    port->os = os;

    if (os->mutex_create != NULL) {
        port->mutex = os->mutex_create();
        if (port->mutex == NULL) {
            free(port);
            return RTCP_ENOMEM;
        }
    }

    if (airplay_control_cipher_create(read_key, write_key, &port->cipher) !=
        AIRPLAY_CONTROL_CIPHER_OK) {
        airplay_control_port_destroy(port);
        return RTCP_EIO;
    }
    if (airplay_control_stream_create(&port->stream) !=
        AIRPLAY_CONTROL_STREAM_OK) {
        airplay_control_port_destroy(port);
        return RTCP_ENOMEM;
    }
    port->plaintext_scratch =
        calloc(AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES, 1U);
    if (port->plaintext_scratch == NULL) {
        airplay_control_port_destroy(port);
        return RTCP_ENOMEM;
    }

    port->stream_id = stream_id;
    port->cseq = initial_cseq;
    port->write_cb = write_cb;
    port->context = context;
    port->active = true;
    *out_port = port;
    return RTCP_OK;
}

void airplay_control_port_destroy(airplay_control_port_t *port)
{
    if (port == NULL) return;
    destroy_channel(port);
    if (port->mutex != NULL && port->os != NULL &&
        port->os->mutex_destroy != NULL) {
        port->os->mutex_destroy(port->mutex);
    }
    mbedtls_platform_zeroize(port, sizeof(*port));
    free(port);
}

bool airplay_control_port_is_active(const airplay_control_port_t *port)
{
    return port != NULL && port->active;
}

static int send_plaintext_locked(airplay_control_port_t *port,
                                 const uint8_t *plaintext,
                                 size_t plaintext_len)
{
    uint8_t frame[AIRPLAY_CONTROL_CIPHER_MESSAGE_CHUNK_BYTES +
                  AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES];
    size_t offset = 0;
    while (offset < plaintext_len) {
        size_t n = plaintext_len - offset;
        if (n > AIRPLAY_CONTROL_CIPHER_MESSAGE_CHUNK_BYTES)
            n = AIRPLAY_CONTROL_CIPHER_MESSAGE_CHUNK_BYTES;
        size_t frame_len = 0;
        airplay_control_cipher_result_t result = airplay_control_cipher_encrypt(
            port->cipher, plaintext + offset, n, frame, sizeof(frame), &frame_len);
        bool written = result == AIRPLAY_CONTROL_CIPHER_OK &&
            port->write_cb(frame, frame_len, port->context);
        mbedtls_platform_zeroize(frame, sizeof(frame));
        if (!written) return RTCP_EIO;
        offset += n;
    }
    return RTCP_OK;
}

static bool send_event_ack(const airplay_rtsp_request_t *request, void *context)
{
    airplay_control_port_t *port = context;
    uint8_t response[AIRPLAY_CONTROL_RESPONSE_BYTES];
    size_t response_len = 0U;
    if (airplay_rtsp_build_response(request, 200U, "OK", NULL, NULL, 0U,
                                    response, sizeof(response),
                                    &response_len) != 0) {
        mbedtls_platform_zeroize(response, sizeof(response));
        return false;
    }
    int result = send_plaintext_locked(port, response, response_len);
    mbedtls_platform_zeroize(response, sizeof(response));
    return result == RTCP_OK;
}

static int process_event_plaintext_locked(airplay_control_port_t *port,
                                          const uint8_t *plaintext,
                                          size_t plaintext_len)
{
    airplay_control_stream_result_t result = airplay_control_stream_feed(
        port->stream, plaintext, plaintext_len, send_event_ack, port);
    if (result == AIRPLAY_CONTROL_STREAM_OK ||
        result == AIRPLAY_CONTROL_STREAM_INCOMPLETE) {
        return RTCP_OK;
    }
    return RTCP_EIO;
}

int airplay_control_port_receive_encrypted(airplay_control_port_t *port,
                                           const uint8_t *bytes,
                                           size_t len)
{
    if (port == NULL || (bytes == NULL && len != 0U)) return RTCP_EINVAL;
    lock(port);
    if (!port->active) {
        unlock(port);
        return RTCP_ESTATE;
    }

    size_t offset = 0U;
    int result = RTCP_OK;
    do {
        size_t consumed = 0U;
        size_t plaintext_len = 0U;
        airplay_control_cipher_result_t cipher_result =
            airplay_control_cipher_decrypt(
                port->cipher, bytes == NULL ? NULL : bytes + offset,
                len - offset, &consumed, port->plaintext_scratch,
                AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES, &plaintext_len);
        offset += consumed;
        if (plaintext_len != 0U) {
            result = process_event_plaintext_locked(
                port, port->plaintext_scratch, plaintext_len);
        }
        mbedtls_platform_zeroize(port->plaintext_scratch,
                                 AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES);
        if (result != RTCP_OK) break;
        if (cipher_result == AIRPLAY_CONTROL_CIPHER_OUTPUT_TOO_SMALL) continue;
        if (cipher_result == AIRPLAY_CONTROL_CIPHER_INCOMPLETE ||
            cipher_result == AIRPLAY_CONTROL_CIPHER_OK) {
            if (offset == len) break;
            if (consumed == 0U) {
                result = RTCP_EIO;
                break;
            }
            continue;
        }
        result = RTCP_EIO;
        break;
    } while (offset <= len);

    if (result != RTCP_OK) destroy_channel(port);
    unlock(port);
    return result;
}

bool airplay_control_port_send_hid_report(airplay_control_port_t *port,
                                          uint32_t hid_uid,
                                          const uint8_t *report,
                                          size_t report_len)
{
    if (port == NULL || report == NULL || report_len == 0U ||
        report_len > 128U) {
        return false;
    }
    uint8_t plaintext[AIRPLAY_CONTROL_MAX_PLAINTEXT];
    size_t plist_len = 0U;
    char header[192];
    bool sent = false;

    if (!airplay_hid_plist_encode(hid_uid, report, report_len, plaintext,
                                  sizeof(plaintext), &plist_len)) {
        return false;
    }
    lock(port);
    if (!port->active) goto done;

    ++port->cseq;
    if (port->cseq == 0U) port->cseq = 1U;
    int header_len;
    if (port->stream_id == 0U) {
        header_len = snprintf(header, sizeof(header),
                              "POST /command RTSP/1.0\r\n"
                              "Content-Type: application/x-apple-binary-plist\r\n"
                              "Content-Length: %u\r\n"
                              "CSeq: %u\r\n\r\n",
                              (unsigned)plist_len, (unsigned)port->cseq);
    } else {
        header_len = snprintf(header, sizeof(header),
                              "POST /command RTSP/1.0\r\n"
                              "X-Apple-StreamID: %u\r\n"
                              "Content-Type: application/x-apple-binary-plist\r\n"
                              "Content-Length: %u\r\n"
                              "CSeq: %u\r\n\r\n",
                              (unsigned)port->stream_id,
                              (unsigned)plist_len, (unsigned)port->cseq);
    }
    if (header_len <= 0 ||
        (size_t)header_len + plist_len > AIRPLAY_CONTROL_MAX_PLAINTEXT) {
        destroy_channel(port);
        goto done;
    }
    memmove(plaintext + header_len, plaintext, plist_len);
    memcpy(plaintext, header, (size_t)header_len);
    sent = send_plaintext_locked(port, plaintext,
                                 (size_t)header_len + plist_len) == RTCP_OK;
    if (!sent) destroy_channel(port);

done:
    mbedtls_platform_zeroize(plaintext, sizeof(plaintext));
    mbedtls_platform_zeroize(header, sizeof(header));
    unlock(port);
    return sent;
}

bool airplay_control_port_request_main_screen_keyframe(
    airplay_control_port_t *port)
{
    if (port == NULL) return false;

    uint8_t plaintext[AIRPLAY_CONTROL_MAX_PLAINTEXT] = {0};
    uint8_t body[96U] = {0};
    char header[192] = {0};
    size_t body_len = 0U;
    bool sent = false;
    bool suppressed = false;
    bool have_session = false;
    const char *failure_reason = "encode_failed";
    uint32_t request_cseq = 0U;
    if (!airplay_command_plist_encode_force_keyframe(body, sizeof(body),
                                                     &body_len)) {
        goto done_without_lock;
    }

    lock(port);
    if (!port->active) {
        failure_reason = "no_event_session";
        goto done;
    }
    have_session = true;

    uint64_t now = port->os->now_ms();
    if (port->keyframe_tick_valid &&
        now - port->keyframe_tick_ms < AIRPLAY_KEYFRAME_REQUEST_INTERVAL_MS) {
        sent = true;
        suppressed = true;
        goto done;
    }

    ++port->cseq;
    if (port->cseq == 0U) port->cseq = 1U;
    request_cseq = port->cseq;
    int header_len = snprintf(header, sizeof(header),
                              "POST /command RTSP/1.0\r\n"
                              "Content-Type: application/x-apple-binary-plist\r\n"
                              "Content-Length: %u\r\n"
                              "CSeq: %u\r\n\r\n",
                              (unsigned)body_len, (unsigned)port->cseq);
    if (header_len <= 0 ||
        (size_t)header_len + body_len > sizeof(plaintext)) {
        destroy_channel(port);
        goto done;
    }
    memcpy(plaintext, header, (size_t)header_len);
    memcpy(plaintext + header_len, body, body_len);
    sent = send_plaintext_locked(port, plaintext,
                                 (size_t)header_len + body_len) == RTCP_OK;
    if (!sent) {
        failure_reason = "event_socket_write_failed";
        destroy_channel(port);
        goto done;
    }
    port->keyframe_tick_ms = now;
    port->keyframe_tick_valid = true;

done:
    if (!suppressed) {
        uint64_t log_now = port->os->now_ms();
        if (!port->keyframe_log_tick_valid ||
            log_now - port->keyframe_log_tick_ms >=
                AIRPLAY_KEYFRAME_LOG_INTERVAL_MS) {
            port->keyframe_log_tick_ms = log_now;
            port->keyframe_log_tick_valid = true;
            if (sent) {
                RTCP_LOGI(TAG, "keyframe_command=sent cseq=%u",
                          (unsigned)request_cseq);
            } else {
                RTCP_LOGW(TAG, "keyframe_command=failed reason=%s event_session=%u",
                          failure_reason, have_session ? 1U : 0U);
            }
        }
    }
    mbedtls_platform_zeroize(plaintext, sizeof(plaintext));
    mbedtls_platform_zeroize(body, sizeof(body));
    mbedtls_platform_zeroize(header, sizeof(header));
    unlock(port);
    return sent;

done_without_lock:
    mbedtls_platform_zeroize(plaintext, sizeof(plaintext));
    mbedtls_platform_zeroize(body, sizeof(body));
    mbedtls_platform_zeroize(header, sizeof(header));
    return false;
}

static bool send_command(airplay_control_port_t *port,const uint8_t *plist,size_t plist_len)
{
    if(!port || !plist || !plist_len || plist_len>2300)return false;
    uint8_t plaintext[2560U];char header[192];bool sent=false;
    lock(port);
    if(!port->active)goto done;
    if(++port->cseq==0)port->cseq=1;
    int n=snprintf(header,sizeof(header),
        "POST /command RTSP/1.0\r\nContent-Type: application/x-apple-binary-plist\r\n"
        "Content-Length: %u\r\nCSeq: %u\r\n\r\n",(unsigned)plist_len,(unsigned)port->cseq);
    if(n<=0 || (size_t)n>=sizeof(header) || (size_t)n+plist_len>sizeof(plaintext))goto done;
    memcpy(plaintext,header,(size_t)n);memcpy(plaintext+n,plist,plist_len);
    sent=send_plaintext_locked(port,plaintext,(size_t)n+plist_len)==RTCP_OK;
    if(!sent)destroy_channel(port);
done:
    mbedtls_platform_zeroize(plaintext,sizeof(plaintext));unlock(port);return sent;
}
bool airplay_control_port_send_iap(airplay_control_port_t *port,const uint8_t *bytes,size_t len)
{
    uint8_t plist[2300];size_t n;
    bool ok=airplay_bplist_build_iap_command(bytes,len,plist,sizeof(plist),&n)==AIRPLAY_BPLIST_OK && send_command(port,plist,n);
    mbedtls_platform_zeroize(plist,sizeof(plist));return ok;
}
bool airplay_control_port_request_siri(airplay_control_port_t *port)
{
    uint8_t plist[256];size_t n;
    for(unsigned action=2;action<=3;++action)
        if(airplay_bplist_build_siri_command(action,plist,sizeof(plist),&n)!=AIRPLAY_BPLIST_OK || !send_command(port,plist,n))return false;
    return true;
}
