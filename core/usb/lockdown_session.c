/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lockdown_session.h"

#include <stdlib.h>
#include <string.h>

#include "lockdown.h"
#include "rtcp/plist.h"
#include "rtcp/log.h"

#define LOCKDOWN_PORT 62078U
#define CARKIT_SERVICE "com.apple.carkit.service"

#define DEFAULT_STEP_TIMEOUT_MS 5000U
#define DEFAULT_PAIR_RETRY_MS 1000U
#define DEFAULT_PAIR_ATTEMPTS 300U

/* The Pair request embeds three PEM certificates, so it needs far more room
 * than a GetValue document.  The reply is still bounded by the lockdown
 * frame limit. */
#define PAIR_REQUEST_MAX (16U * 1024U)

static uint32_t step_timeout(const rtcp_lockdown_session_t *session)
{
    return session->config.step_timeout_ms != 0U
               ? session->config.step_timeout_ms
               : DEFAULT_STEP_TIMEOUT_MS;
}

static uint8_t *reply_buffer(rtcp_lockdown_session_t *session)
{
    return session->scratch;
}

static uint8_t *request_buffer(rtcp_lockdown_session_t *session)
{
    return session->scratch + RTCP_LOCKDOWN_FRAME_MAX;
}

static uint8_t *device_key_buffer(rtcp_lockdown_session_t *session)
{
    return session->scratch + RTCP_LOCKDOWN_FRAME_MAX + PAIR_REQUEST_MAX;
}

static int exchange(rtcp_lockdown_session_t *session,
                    const rtcp_stream_t *stream, const char *operation,
                    const uint8_t *request, size_t request_length,
                    size_t *reply_length)
{
    session->operation = operation;
    ++session->exchange_count;
    session->last_request_length = request_length;
    session->last_reply_length = 0U;
    *reply_length = 0U;
    RTCP_LOGI("lockdown", "request=%s bytes=%u timeout=%u", operation,
              (unsigned)request_length, (unsigned)step_timeout(session));
    int ret = rtcp_lockdown_exchange(stream, request, request_length,
                                     reply_buffer(session),
                                     RTCP_LOCKDOWN_FRAME_MAX, reply_length,
                                     step_timeout(session));
    session->last_reply_length = *reply_length;
    session->last_exchange_result = ret;
    RTCP_LOGI("lockdown", "reply=%s ret=%d bytes=%u", operation, ret,
              (unsigned)*reply_length);
    return ret;
}

/* RTCP_OK when the reply carries no "Error", RTCP_EIO with the phone's error
 * text stored in session->remote_error otherwise. */
static int reply_error(rtcp_lockdown_session_t *session,
                       const uint8_t *reply, size_t reply_length)
{
    char error[RTCP_LOCKDOWN_SESSION_ERROR_MAX];
    int ret = rtcp_plist_get_error(reply, reply_length, error,
                                   sizeof(error));

    if (ret == RTCP_ENOENT) {
        return RTCP_OK;
    }
    if (ret != RTCP_OK) {
        return RTCP_EIO;
    }
    memcpy(session->remote_error, error, strlen(error) + 1U);
    return RTCP_EIO;
}

/* One Pair attempt on one plaintext connection.  Returns RTCP_OK when the
 * phone accepted the record, RTCP_EIO otherwise; *pending tells the caller
 * that the phone is waiting for the user and the attempt should be repeated
 * after a reconnect. */
static int pair_attempt(rtcp_lockdown_session_t *session,
                        const rtcp_stream_t *stream, bool *pending)
{
    rtcp_lockdown_session_config_t *config = &session->config;
    rtcp_pair_record_t *record = config->record;
    uint8_t *request = request_buffer(session);
    size_t request_length;
    size_t reply_length = 0U;
    int ret;

    *pending = false;

    request_length = rtcp_lockdown_build_set_value(
        request, PAIR_REQUEST_MAX, config->label, "UntrustedHostBUID",
        config->system_buid);
    if (request_length == 0U) {
        return RTCP_EINVAL;
    }
    ret = exchange(session, stream, "SetValue/UntrustedHostBUID", request,
                   request_length, &reply_length);
    if (ret != RTCP_OK) {
        return ret;
    }
    ret = reply_error(session, reply_buffer(session), reply_length);
    if (ret != RTCP_OK) {
        return ret;
    }

    if (record->root_certificate_length == 0U) {
        uint8_t *device_key = device_key_buffer(session);
        size_t device_key_length = 0U;
        char wifi_address[sizeof(record->wifi_address)];
        char value[sizeof(record->wifi_address)];

        request_length = rtcp_lockdown_build_get_value(
            request, PAIR_REQUEST_MAX, config->label, "DevicePublicKey");
        if (request_length == 0U) {
            return RTCP_EINVAL;
        }
        ret = exchange(session, stream, "GetValue/DevicePublicKey", request, request_length,
                       &reply_length);
        if (ret != RTCP_OK) {
            return ret;
        }
        ret = reply_error(session, reply_buffer(session), reply_length);
        if (ret != RTCP_OK) {
            return ret;
        }
        ret = rtcp_plist_get_data(reply_buffer(session), reply_length, "Value",
                                  device_key, RTCP_PAIR_DEVICE_KEY_MAX,
                                  &device_key_length);
        if (ret != RTCP_OK || device_key_length == 0U) {
            return RTCP_EIO;
        }

        request_length = rtcp_lockdown_build_get_value(
            request, PAIR_REQUEST_MAX, config->label, "WiFiAddress");
        if (request_length == 0U) {
            return RTCP_EINVAL;
        }
        ret = exchange(session, stream, "GetValue/WiFiAddress", request, request_length,
                       &reply_length);
        if (ret != RTCP_OK) {
            return ret;
        }
        ret = reply_error(session, reply_buffer(session), reply_length);
        if (ret != RTCP_OK) {
            return ret;
        }
        ret = rtcp_plist_get_string(reply_buffer(session), reply_length,
                                    "Value", value, sizeof(value));
        if (ret != RTCP_OK) {
            return RTCP_EIO;
        }
        memcpy(wifi_address, value, strlen(value) + 1U);

        session->operation = "GeneratePairRecord";
        RTCP_LOGI("lockdown", "generating pair record");
        ret = rtcp_pair_record_generate(record, device_key, device_key_length,
                                        wifi_address, config->host_id,
                                        config->system_buid, config->rng,
                                        config->rng_context);
        RTCP_LOGI("lockdown", "pair record generation ret=%d", ret);
        if (ret != RTCP_OK) {
            return ret;
        }
    }

    request_length = rtcp_lockdown_build_pair(
        request, PAIR_REQUEST_MAX, config->label, config->host_id,
        config->system_buid, record->device_certificate,
        record->device_certificate_length, record->host_certificate,
        record->host_certificate_length, record->root_certificate,
        record->root_certificate_length);
    if (request_length == 0U) {
        return RTCP_EINVAL;
    }
    ret = exchange(session, stream, "Pair", request, request_length, &reply_length);
    if (ret != RTCP_OK) {
        return ret;
    }
    ret = reply_error(session, reply_buffer(session), reply_length);
    if (ret != RTCP_OK &&
        strcmp(session->remote_error, "PairingDialogResponsePending") == 0) {
        session->remote_error[0] = '\0';
        *pending = true;
        return RTCP_EIO;
    }
    return ret;
}

static int pair(rtcp_lockdown_session_t *session)
{
    rtcp_lockdown_session_config_t *config = &session->config;
    uint32_t attempts = config->pair_attempts != 0U ? config->pair_attempts
                                                    : DEFAULT_PAIR_ATTEMPTS;
    uint32_t retry_ms = config->pair_retry_ms != 0U ? config->pair_retry_ms
                                                    : DEFAULT_PAIR_RETRY_MS;
    uint32_t attempt;

    session->stage = RTCP_LOCKDOWN_STAGE_PAIR;
    for (attempt = 0U; attempt < attempts; ++attempt) {
        rtcp_stream_t raw;
        bool pending = false;
        int ret;

        memset(&raw, 0, sizeof(raw));
        session->operation = "OpenLockdown";
        ret = config->open_connection(config->connection_user, LOCKDOWN_PORT,
                                      &raw);
        if (ret != RTCP_OK) {
            return ret;
        }
        ret = pair_attempt(session, &raw, &pending);
        config->close_connection(config->connection_user, &raw);
        if (ret == RTCP_OK) {
            session->paired = true;
            return RTCP_OK;
        }
        if (!pending) {
            return ret;
        }
        if (attempt + 1U < attempts && config->sleep_ms != NULL) {
            config->sleep_ms(config->sleep_user, retry_ms);
        }
    }
    return RTCP_ETIMEDOUT;
}

static int start_service(rtcp_lockdown_session_t *session)
{
    rtcp_lockdown_session_config_t *config = &session->config;
    uint8_t *request = request_buffer(session);
    size_t request_length;
    size_t reply_length = 0U;
    int64_t port = 0;
    bool enable_ssl = false;
    int ret;

    session->stage = RTCP_LOCKDOWN_STAGE_START_SESSION;
    session->operation = "OpenSessionConnection";
    ret = config->open_connection(config->connection_user, LOCKDOWN_PORT,
                                  &session->control_raw);
    if (ret != RTCP_OK) {
        return ret;
    }

    request_length = rtcp_lockdown_build_start_session(
        request, PAIR_REQUEST_MAX, config->label, config->host_id,
        config->system_buid);
    if (request_length == 0U) {
        return RTCP_EINVAL;
    }
    ret = exchange(session, &session->control_raw, "StartSession", request,
                   request_length, &reply_length);
    if (ret != RTCP_OK) {
        return ret;
    }
    ret = reply_error(session, reply_buffer(session), reply_length);
    if (ret != RTCP_OK) {
        return ret;
    }
    ret = rtcp_plist_get_boolean(reply_buffer(session), reply_length,
                                 "EnableSessionSSL", &enable_ssl);
    if (ret != RTCP_OK || !enable_ssl) {
        return RTCP_EIO;
    }

    session->stage = RTCP_LOCKDOWN_STAGE_SESSION_TLS;
    session->operation = "SessionTLS";
    ret = rtcp_lockdown_tls_open(&session->control_tls, &session->control_raw,
                                 config->record, config->rng,
                                 config->rng_context, step_timeout(session));
    if (ret != RTCP_OK) {
        return ret;
    }
    session->control = rtcp_lockdown_tls_stream(session->control_tls);

    session->stage = RTCP_LOCKDOWN_STAGE_START_SERVICE;
    request_length = rtcp_lockdown_build_start_service(
        request, PAIR_REQUEST_MAX, CARKIT_SERVICE);
    if (request_length == 0U) {
        return RTCP_EINVAL;
    }
    ret = exchange(session, &session->control, "StartService", request,
                   request_length, &reply_length);
    if (ret != RTCP_OK) {
        return ret;
    }
    ret = reply_error(session, reply_buffer(session), reply_length);
    if (ret != RTCP_OK) {
        return ret;
    }
    ret = rtcp_plist_get_integer(reply_buffer(session), reply_length, "Port",
                                 &port);
    if (ret != RTCP_OK || port < 1 || port > 0xffff) {
        return RTCP_EIO;
    }
    ret = rtcp_plist_get_boolean(reply_buffer(session), reply_length,
                                 "EnableServiceSSL", &enable_ssl);
    if (ret != RTCP_OK && ret != RTCP_ENOENT) {
        return RTCP_EIO;
    }
    session->service_port = (uint16_t)port;
    session->service_ssl = ret == RTCP_OK ? enable_ssl : false;

    session->stage = RTCP_LOCKDOWN_STAGE_SERVICE;
    ret = config->open_connection(config->connection_user,
                                  session->service_port,
                                  &session->service_raw);
    if (ret != RTCP_OK) {
        return ret;
    }
    if (session->service_ssl) {
        ret = rtcp_lockdown_tls_open(&session->service_tls,
                                     &session->service_raw, config->record,
                                     config->rng, config->rng_context,
                                     step_timeout(session));
        if (ret != RTCP_OK) {
            return ret;
        }
        session->service = rtcp_lockdown_tls_stream(session->service_tls);
    } else {
        session->service = session->service_raw;
    }
    return RTCP_OK;
}

int rtcp_lockdown_session_open(rtcp_lockdown_session_t *session,
                               const rtcp_lockdown_session_config_t *config)
{
    int ret;

    if (session == NULL || config == NULL || config->record == NULL ||
        config->rng == NULL || config->label == NULL ||
        config->label[0] == '\0' || config->open_connection == NULL ||
        config->close_connection == NULL) {
        return RTCP_EINVAL;
    }

    memset(session, 0, sizeof(*session));
    session->config = *config;

    if (session->config.host_id[0] == '\0' &&
        rtcp_pair_generate_uuid(session->config.host_id,
                                sizeof(session->config.host_id),
                                config->rng, config->rng_context) != RTCP_OK) {
        return RTCP_EIO;
    }
    if (session->config.system_buid[0] == '\0' &&
        rtcp_pair_generate_uuid(session->config.system_buid,
                                sizeof(session->config.system_buid),
                                config->rng, config->rng_context) != RTCP_OK) {
        return RTCP_EIO;
    }
    if (session->config.step_timeout_ms == 0U) {
        session->config.step_timeout_ms = DEFAULT_STEP_TIMEOUT_MS;
    }
    if (session->config.pair_retry_ms == 0U) {
        session->config.pair_retry_ms = DEFAULT_PAIR_RETRY_MS;
    }
    if (session->config.pair_attempts == 0U) {
        session->config.pair_attempts = DEFAULT_PAIR_ATTEMPTS;
    }

    session->scratch_length = RTCP_LOCKDOWN_FRAME_MAX + PAIR_REQUEST_MAX +
                              RTCP_PAIR_DEVICE_KEY_MAX;
    session->scratch = malloc(session->scratch_length);
    if (session->scratch == NULL) {
        return RTCP_ENOMEM;
    }

    ret = pair(session);
    if (ret == RTCP_OK) {
        ret = start_service(session);
    }
    if (ret != RTCP_OK) {
        rtcp_lockdown_session_close(session);
    }
    return ret;
}

rtcp_stream_t rtcp_lockdown_session_service(
    const rtcp_lockdown_session_t *session)
{
    rtcp_stream_t stream = { NULL, NULL };

    if (session != NULL) {
        stream = session->service;
    }
    return stream;
}

void rtcp_lockdown_session_close(rtcp_lockdown_session_t *session)
{
    if (session == NULL) {
        return;
    }
    if (session->service_tls != NULL) {
        rtcp_lockdown_tls_close(session->service_tls);
        session->service_tls = NULL;
    }
    if (session->service_raw.ops != NULL &&
        session->config.close_connection != NULL) {
        session->config.close_connection(session->config.connection_user,
                                         &session->service_raw);
    }
    memset(&session->service_raw, 0, sizeof(session->service_raw));
    memset(&session->service, 0, sizeof(session->service));

    if (session->control_tls != NULL) {
        rtcp_lockdown_tls_close(session->control_tls);
        session->control_tls = NULL;
    }
    if (session->control_raw.ops != NULL &&
        session->config.close_connection != NULL) {
        session->config.close_connection(session->config.connection_user,
                                         &session->control_raw);
    }
    memset(&session->control_raw, 0, sizeof(session->control_raw));
    memset(&session->control, 0, sizeof(session->control));

    free(session->scratch);
    session->scratch = NULL;
    session->scratch_length = 0U;
}

const char *rtcp_lockdown_stage_name(rtcp_lockdown_stage_t stage)
{
    switch (stage) {
    case RTCP_LOCKDOWN_STAGE_IDLE:
        return "idle";
    case RTCP_LOCKDOWN_STAGE_PAIR:
        return "pair";
    case RTCP_LOCKDOWN_STAGE_START_SESSION:
        return "start-session";
    case RTCP_LOCKDOWN_STAGE_SESSION_TLS:
        return "session-tls";
    case RTCP_LOCKDOWN_STAGE_START_SERVICE:
        return "start-service";
    case RTCP_LOCKDOWN_STAGE_SERVICE:
        return "service";
    default:
        return "unknown";
    }
}
