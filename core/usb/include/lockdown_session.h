/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Wired lockdown pairing and CarPlay service session.
 *
 * This is the sequence the wired reference uses over the USBMUX lockdown
 * port, independent of how the byte stream to the phone is produced:
 *
 *   1. SetValue UntrustedHostBUID, read DevicePublicKey and WiFiAddress,
 *      generate the local pair record and send Pair (plaintext).
 *   2. While the phone shows its trust dialog, reconnect and retry; the
 *      record is generated once and reused.
 *   3. StartSession on a fresh connection, then wrap it in TLS with the
 *      record's root identity (EnableSessionSSL must be true).
 *   4. StartService "com.apple.carkit.service" over the TLS session, open a
 *      second connection to the reported port and optionally TLS-wrap it.
 *
 * The session owns those connections through the caller's open/close
 * callbacks.  It does not know USBMUX or USB: the board passes a factory
 * that opens a stream to a phone port, the host test passes a fake phone.
 */
#ifndef RTCP_LOCKDOWN_SESSION_H
#define RTCP_LOCKDOWN_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lockdown_pair.h"
#include "lockdown_tls.h"
#include "rtcp/rtcp.h"
#include "rtcp/stream.h"

#define RTCP_LOCKDOWN_SESSION_ERROR_MAX 40U

typedef enum {
    RTCP_LOCKDOWN_STAGE_IDLE = 0,
    RTCP_LOCKDOWN_STAGE_PAIR,
    RTCP_LOCKDOWN_STAGE_START_SESSION,
    RTCP_LOCKDOWN_STAGE_SESSION_TLS,
    RTCP_LOCKDOWN_STAGE_START_SERVICE,
    RTCP_LOCKDOWN_STAGE_SERVICE,
} rtcp_lockdown_stage_t;

typedef struct {
    /* Open a byte stream to `port` on the phone. */
    int (*open_connection)(void *user, uint16_t port, rtcp_stream_t *stream);
    /* Close a stream previously returned by open_connection(). */
    void (*close_connection)(void *user, const rtcp_stream_t *stream);
    void *connection_user;

    /* Wait between pairing retries; NULL skips the wait. */
    void (*sleep_ms)(void *user, uint32_t ms);
    void *sleep_user;

    rtcp_pair_rng_fn rng;
    void *rng_context;

    /* Host label sent in every lockdown request. */
    const char *label;

    /* Leave empty to generate on open(); the generated values are visible in
     * the session afterwards.  They must be valid bare plist tokens. */
    char host_id[RTCP_PAIR_UUID_CHARS];
    char system_buid[RTCP_PAIR_UUID_CHARS];

    /* Caller-owned storage for the generated record. */
    rtcp_pair_record_t *record;

    /* Per-read timeout; 0 uses 5000 ms. */
    uint32_t step_timeout_ms;
    /* Delay between trust-dialog retries; 0 uses 1000 ms. */
    uint32_t pair_retry_ms;
    /* Maximum Pair attempts; 0 uses 300 (five minutes at the default
     * retry delay). */
    uint32_t pair_attempts;
} rtcp_lockdown_session_config_t;

typedef struct {
    rtcp_lockdown_session_config_t config;

    rtcp_stream_t control;      /* TLS stream once StartSession enabled it */
    rtcp_stream_t control_raw;  /* connection owned by open_connection */
    rtcp_lockdown_tls_t *control_tls;

    rtcp_stream_t service;
    rtcp_stream_t service_raw;
    rtcp_lockdown_tls_t *service_tls;

    uint16_t service_port;
    bool service_ssl;
    bool paired;

    rtcp_lockdown_stage_t stage;
    const char *operation; /* static diagnostic name, preserved after failure */
    uint32_t exchange_count;
    size_t last_request_length;
    size_t last_reply_length;
    int last_exchange_result;
    char remote_error[RTCP_LOCKDOWN_SESSION_ERROR_MAX];

    uint8_t *scratch; /* reply frame plus device-key scratch */
    size_t scratch_length;
} rtcp_lockdown_session_t;

/* Run the full pairing/session/service sequence.  `config->record` must stay
 * valid until close; the generated host_id/system_buid are stored in the
 * session.  On failure `session->stage` names the step that failed and
 * `remote_error` carries the phone's Error value when there was one.
 * Returns RTCP_OK, RTCP_EINVAL, RTCP_ENOMEM, RTCP_ETIMEDOUT or RTCP_EIO. */
int rtcp_lockdown_session_open(rtcp_lockdown_session_t *session,
                               const rtcp_lockdown_session_config_t *config);

/* Service byte stream; a TLS stream when EnableServiceSSL was set.  Valid
 * until rtcp_lockdown_session_close().  ops is NULL when no service is
 * open. */
rtcp_stream_t rtcp_lockdown_session_service(
    const rtcp_lockdown_session_t *session);

/* Release the service, the lockdown session and every raw connection.
 * Accepts NULL and a partially opened session. */
void rtcp_lockdown_session_close(rtcp_lockdown_session_t *session);

const char *rtcp_lockdown_stage_name(rtcp_lockdown_stage_t stage);

#endif /* RTCP_LOCKDOWN_SESSION_H */
