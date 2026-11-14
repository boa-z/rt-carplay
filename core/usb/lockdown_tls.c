/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lockdown_tls.h"

#include <stdlib.h>
#include <string.h>

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "rtcp/log.h"

/* mbedTLS may report WANT_READ/WANT_WRITE through a blocking callback only
 * on benign renegotiation or shutdown races; bound the retries so a broken
 * transport cannot spin forever. */
#define TLS_WANT_RETRIES 16

struct rtcp_lockdown_tls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context key;

    rtcp_stream_t transport;
    rtcp_pair_rng_fn rng;
    void *rng_context;
    uint32_t timeout_ms;
    int session_ready;
};

static int tls_rng(void *context, unsigned char *output, size_t length)
{
    rtcp_lockdown_tls_t *tls = context;

    return tls->rng(tls->rng_context, output, length);
}

static int tls_send(void *context, const unsigned char *buffer, size_t length)
{
    rtcp_lockdown_tls_t *tls = context;
    int ret;

    ret = tls->transport.ops->write(tls->transport.context, buffer, length);
    if (ret != RTCP_OK) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return (int)length;
}

static int tls_recv(void *context, unsigned char *buffer, size_t length)
{
    rtcp_lockdown_tls_t *tls = context;
    int ret;

    ret = tls->transport.ops->read(tls->transport.context, buffer, length,
                                   tls->timeout_ms);
    if (ret == RTCP_ETIMEDOUT) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    if (ret < 0) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    /* 0 is EOF, which mbedTLS handles as a closed peer. */
    return ret;
}

static int tls_map_error(int ret)
{
    switch (ret) {
    case MBEDTLS_ERR_SSL_TIMEOUT:
        return RTCP_ETIMEDOUT;
    case MBEDTLS_ERR_SSL_ALLOC_FAILED:
        return RTCP_ENOMEM;
    default:
        return RTCP_EIO;
    }
}

static int tls_stream_read(void *context, void *buffer, size_t length,
                           uint32_t timeout_ms)
{
    rtcp_lockdown_tls_t *tls = context;
    int retries = 0;

    if (length == 0U) {
        return 0;
    }
    if (timeout_ms != 0U) {
        tls->timeout_ms = timeout_ms;
    }

    for (;;) {
        int ret = mbedtls_ssl_read(&tls->ssl, buffer, length);

        if (ret > 0) {
            return ret;
        }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return 0;
        }
        if (ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            return RTCP_ETIMEDOUT;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (++retries <= TLS_WANT_RETRIES) {
                continue;
            }
            return RTCP_ETIMEDOUT;
        }
        return RTCP_EIO;
    }
}

static int tls_stream_write(void *context, const void *data, size_t length)
{
    rtcp_lockdown_tls_t *tls = context;
    const unsigned char *cursor = data;
    size_t written = 0U;
    int retries = 0;

    while (written < length) {
        int ret = mbedtls_ssl_write(&tls->ssl, cursor + written,
                                    length - written);

        if (ret > 0) {
            written += (size_t)ret;
            retries = 0;
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (++retries <= TLS_WANT_RETRIES) {
                continue;
            }
            return RTCP_ETIMEDOUT;
        }
        return RTCP_EIO;
    }
    return RTCP_OK;
}

static const rtcp_stream_ops_t tls_stream_ops = {
    .read = tls_stream_read,
    .write = tls_stream_write,
};

int rtcp_lockdown_tls_open(rtcp_lockdown_tls_t **tls,
                           const rtcp_stream_t *transport,
                           const rtcp_pair_record_t *record,
                           rtcp_pair_rng_fn rng, void *rng_context,
                           uint32_t timeout_ms)
{
    rtcp_lockdown_tls_t *session;
    int ret;

    if (tls == NULL || transport == NULL || transport->ops == NULL ||
        transport->ops->read == NULL || transport->ops->write == NULL ||
        record == NULL || rng == NULL ||
        record->root_certificate_length == 0U ||
        record->root_private_key_length == 0U) {
        if (tls != NULL) {
            *tls = NULL;
        }
        return RTCP_EINVAL;
    }
    *tls = NULL;

    session = calloc(1U, sizeof(*session));
    if (session == NULL) {
        return RTCP_ENOMEM;
    }
    mbedtls_ssl_init(&session->ssl);
    mbedtls_ssl_config_init(&session->config);
    mbedtls_x509_crt_init(&session->certificate);
    mbedtls_pk_init(&session->key);

    session->transport = *transport;
    session->rng = rng;
    session->rng_context = rng_context;
    session->timeout_ms = timeout_ms != 0U ? timeout_ms
                                           : RTCP_LOCKDOWN_TLS_TIMEOUT_MS;

    ret = mbedtls_x509_crt_parse(&session->certificate,
                                 record->root_certificate,
                                 record->root_certificate_length + 1U);
    if (ret != 0) {
        goto fail;
    }
    ret = mbedtls_pk_parse_key(&session->key, record->root_private_key,
                               record->root_private_key_length + 1U, NULL,
                               0U);
    if (ret != 0) {
        goto fail;
    }

    ret = mbedtls_ssl_config_defaults(&session->config, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        goto fail;
    }
    /* Pairing, not a CA chain, establishes who the peer is. */
    mbedtls_ssl_conf_authmode(&session->config, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&session->config, tls_rng, session);
    /* The wired accessory stack speaks TLS 1.2. */
    mbedtls_ssl_conf_min_version(&session->config, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&session->config, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    ret = mbedtls_ssl_conf_own_cert(&session->config, &session->certificate,
                                    &session->key);
    if (ret != 0) {
        goto fail;
    }

    ret = mbedtls_ssl_setup(&session->ssl, &session->config);
    if (ret != 0) {
        goto fail;
    }
    mbedtls_ssl_set_bio(&session->ssl, session, tls_send, tls_recv, NULL);

    ret = mbedtls_ssl_handshake(&session->ssl);
    if (ret != 0) {
        goto fail_handshake;
    }

    session->session_ready = 1;
    *tls = session;
    return RTCP_OK;

fail_handshake:
    RTCP_LOGW("lockdown_tls", "handshake failed raw=%d mapped=%d",
              ret, tls_map_error(ret));
    rtcp_lockdown_tls_close(session);
    return tls_map_error(ret);
fail:
    rtcp_lockdown_tls_close(session);
    return RTCP_EIO;
}

rtcp_stream_t rtcp_lockdown_tls_stream(rtcp_lockdown_tls_t *tls)
{
    rtcp_stream_t stream = { NULL, NULL };

    if (tls != NULL && tls->session_ready) {
        stream.ops = &tls_stream_ops;
        stream.context = tls;
    }
    return stream;
}

void rtcp_lockdown_tls_close(rtcp_lockdown_tls_t *tls)
{
    if (tls == NULL) {
        return;
    }
    if (tls->session_ready) {
        (void)mbedtls_ssl_close_notify(&tls->ssl);
        tls->session_ready = 0;
    }
    mbedtls_ssl_free(&tls->ssl);
    mbedtls_ssl_config_free(&tls->config);
    mbedtls_x509_crt_free(&tls->certificate);
    mbedtls_pk_free(&tls->key);
    free(tls);
}
